# Pipeline 模型配置改造设计

> **状态**：设计已确认，待实施。
> **基线**：`main@cda1f5c`。
> **实施**：按[实施总说明](PIPELINE_REFACTOR_GUIDE.md)的步骤进行；本文中关于 PR 划分和门禁的说法与总说明不一致时，以总说明为准。
> **性质**：跨多个 PR 的工作计划，规则见 [plans/README](README.md) 与 [CONTRIBUTING](../CONTRIBUTING.md) §3。
> 各阶段 PR 的描述引用本文对应章节；最后一个阶段合入时删除本文，形成的规则写进现行指南。
> **目标**：简化系统、降低开发心智、提升易用性。每项改动都应能落到下面某一条：概念更少、写法只有一种、名字一看就懂、出错更早。
> **不做兼容**：项目尚未发布，没有兼容使用方。只实现新方案：不保留旧字段别名、不识别旧格式、不提交迁移代码、不写针对旧名字的测试；已有的此类代码一并删除（第 11 节）。
> **依赖**：[Pipeline I/O 改造设计](PIPELINE_IO_DESIGN.md)。阶段 1 依赖它的阶段 0（参数声明下沉到 `contracts/`）；阶段 2 依赖它的阶段 2（两者都改 Core 校验器、部署准备、命令行工具、Studio 和全部方案，先后实施以免冲突）。
> **分层**：四层依赖方向不变。文件路径由接入层解析；模型条目由 Core 校验；参数声明机制在 `contracts/`；模型、后端只读自己名下的参数。
> **不在范围**：
> - `.conf`、`pipe_path`、Operator 的 Create 参数：外部契约，不改；
> - `pipeline` 节点条目的写法与连线方式：见 [Pipeline 节点与连线改造设计](PIPELINE_NODE_DESIGN.md)。本文只改节点上与模型有关的参数（第 8 节）；
> - 新功能：思考模式、LoRA 适配器。本文只确定它们出现时放在哪里（第 3 节、第 17 节）。

## 1. 目标

改造完成后：

1. 模型条目由类别 `type`、模型名 `name`、权重文件 `file`、模型参数 `params` 和后端 `backend` 组成。写法与 `io`、节点一致：每项写 `type` 加 `name`，参数放在 `params`。用哪个模型实现由类别和后端决定，配置中不写实现名。
2. 模型、后端参数与节点、converter 用同一套声明：默认值、范围、说明在代码里只写一次；配置只写要改的项；代码按结构体成员读取；一个函数完成校验，预检和运行时共用。
3. 参数按一条规则归属：同一个已加载的模型、不同节点能取不同值的参数放节点，其余放模型或后端。
4. 所有文件名相对 Pipeline JSON 所在目录，由接入层统一解析；方案与权重放在同一目录 `configs/`。
5. 能从模型文件读到的事实不必手写。

改造后的配置（文档问答，cpu）：

```json
"models": [
  {
    "type": "embedding",
    "name": "embed_model",
    "file": "bge_base_zh_v1.5.onnx",
    "params":  { "tokenizer_file": "bge_base_zh_v1.5_vocab.txt" },
    "backend": { "type": "onnxruntime" }
  },
  {
    "type": "llm",
    "name": "llm_model",
    "file": "qwen2.5-0.5b-instruct-q4_k_m.gguf",
    "backend": { "type": "llama_cpp", "params": { "context_size": 1024 } }
  }
]
```

对开发者的变化：

| | 现在 | 改造后 |
| --- | --- | --- |
| 模型条目 | 6 个键：`model_id`、`model_type`（实现名）、`backend`、`model_path`、`model_config`、`backend_config` | 5 个键：`type`（类别）、`name`、`file`、`params`（可省略）、`backend`（其中 `type` 为后端名，`params` 可省略） |
| 看出模型属于哪一类 | 要知道实现名对应的能力 | `type` 直接写类别；实现由类别和后端决定，不用写 |
| 向量归一化 | 写在每个向量节点上，查询和语料两个节点要分别写成一样 | 向量模型的参数，绑定同一模型的节点自然一致 |
| 参数声明 | 三种：8 元组位置参数、手写解析函数、手写循环 | 一种：`Parameters` 加 `Field`，与节点、converter 相同 |
| 代码取参数 | `ConfigValueOrDefault(cfg, Def().config_fields, "名字")`，按字符串查找 | `ctx.Params<Params>().max_tokens` |
| 参数校验 | 预检、工厂、`Create` 最多三遍 | 一个函数，预检与运行时共用；`Create` 只做依赖已加载内容的检查 |
| 文件路径 | 三套规则，词表的绝对路径不检查越界 | 一条规则，接入层一处实现 |
| 系统提示词、随机种子、转写语言 | 固定在模型上，想换一个值就要再加载一份权重 | 节点参数 |
| 批大小 | 模型、后端各写一份，生效值取较小者 | 只在后端 |
| 向量维度等模型事实 | 必须手写 | 模型文件能提供时自动读取 |
| 配置里的默认值 | `configs/` 的 183 个参数值中有 137 个等于默认值 | 只写与默认值不同的项 |
| 没有节点使用的模型 | 照样加载 | 报错 |

## 2. 现状与问题

| 编号 | 问题 | 依据 |
| --- | --- | --- |
| M1 | 参数有三种写法 | 6 个模型与 onnxruntime、kite_llm 用 8 元组声明，再用 `ConfigValueOrDefault` 按字符串取值（33 处）；llama_cpp 手写 `ParseLlamaCppConfig`，默认值写在常量、结构体、`config.value(...)` 三处；whisper_cpp 在 `Load` 里手写循环检查未知字段和范围 |
| M2 | 同一参数最多校验三遍 | `PipelineValidator` 归一化并调用 `validate_config`；`ModelRuntimeFactory` 把模型参数再归一化一次、再调用一次 `validate_config`；各模型 `Create` 再查范围和枚举（`whisper_asr` 的 `language`、`bge_reranker` 的 `max_length`、`generated_text_embedding` 的维数等）；kite_llm、whisper_cpp 的 `Load` 再查未知字段。现行指南还要求 `Create` "复用相同语义检查" |
| M3 | 选择与参数分开写，名字不对称 | `model_type` 配 `model_config`，`backend` 配 `backend_config`；JSON 叫 `backend`，Definition、Catalog、`describe-backend` 叫 `backend_type` |
| M4 | 配置看不出模型属于哪一类 | `model_type` 是实现名（`bge_embedding`）；类别在代码里叫 `capability`，不出现在配置中 |
| M5 | 批大小写在两处 | `bge_embedding`、`bge_reranker` 的 `model_config.max_batch_size` 与 onnxruntime 的 `backend_config.max_batch_size`，生效值取较小者；配置中 20 处全是默认值 4。现行指南已写明 "Batch policy belongs to the session, not IModel" |
| M6 | 文件路径三套规则，一处可越界 | `model_path` 相对 Create 的部署根，允许根内的绝对路径，接入层与 Core 各查一次；`tokenizer_file` 相对权重所在目录，写绝对路径时不检查是否越界（`bert_model_support.cpp` 的 `ResolveTokenizerResourcePath`）；kite 的 `run_config_file` 相对权重所在目录，禁止绝对路径（`ResolveRunConfig`） |
| M7 | 方案和权重分在两个目录 | 方案在 `configs/`，权重在 `models/`，方案里写 `./models/` 或 `models/` 前缀（两种都有）。外部约定是"JSON 所在目录拼接文件名" |
| M8 | 提示词有两处可写 | 模型的 `system_prompt` 与节点的模板。`dialogue_audit` 的 default 变体把角色指令写在节点模板里，kite 变体写在 `system_prompt` 里；PromptGuidedLlmNode 的参数说明专门提示"system 角色请使用 model_config.system_prompt" |
| M9 | 每次调用可以不同的参数被固定在模型上 | `system_prompt`、`random_seed`、whisper 的 `language`：两个节点要取不同的值，只能把同一份权重加载两次 |
| M10 | 能从模型读到的事实必须手写 | `bge_embedding.embedding_dim` 必填，而 ONNX 输出张量的形状里就有；现在只拿它来核对配置 |
| M11 | 内容冗余、容易误导 | 137/183 个值等于默认值（附录 A）；`pipeline_doc_qa_rerank_kite.json` 里叫 `llm_model_llamacpp` 的模型用的是 `kite_llm`；whisper、kite 的参数说明是英文 |
| M12 | 遗留与无效代码 | `bge_embedding` 的 `Create` 专门拒绝旧字段 `normalize`，而未知字段早已被通用规则拒绝，这段执行不到；开发指南仍保留对应的迁移说明 |
| M13 | 没有节点使用的模型照样加载 | Core 不检查；端侧多加载一份权重只会浪费内存 |
| M14 | 向量归一化写在节点上 | TextEmbeddingNode 的 `normalize`。同一个向量模型被语料、查询两个节点使用时，两边必须一致，否则相似度没有意义；现在要靠两处写成一样 |

## 3. 参数归属规则

每个参数按下面两问决定位置：

1. **同一个已加载的模型，不同节点能不能合理地取不同的值？**
   - 能：放节点，作为这一类能力的调用参数，即使只有部分实现支持。
   - 不能：放模型或后端。不能的情况有三种：由权重决定（分词约定、张量形状）；各节点的结果要互相比较（同一向量空间）；加载时就分配（上下文容量、线程）。
2. **放模型还是后端**：模型语义（分词、提示格式、池化、预处理）放模型；运行资源（上下文容量、线程、设备、厂商运行配置）放后端。

只有部分实现支持的节点参数：不写时不干预，绑定不支持它的模型也照常工作；节点明确要求、而所绑模型做不到时，在 Create 阶段报错（8.3），不等到处理请求时才失败。

kiteLLM 的接口也是这样划分的：加载时设置的是 `kiteLLM_Parameter_*`（设备、run config 等），每次任务设置的是 `kiteLLM_TaskInput_*`（温度、top_k、生成长度、思考模式等）。

按规则核对现有参数：

| 参数 | 不同节点能否取不同值 | 改造后位置 |
| --- | --- | --- |
| 向量模型的 `pooling`、`prompt_prefix`、`prompt_suffix` | 不能：查询向量与语料向量要比较。现有 9 份含向量模型的方案，都是同一个模型被语料、查询两个 TextEmbeddingNode 使用 | 模型（不变） |
| 向量的 `normalize` | 不能：理由同上 | **模型**（从 TextEmbeddingNode 移来，8.4） |
| `add_bos`、`do_lower_case`、编码长度 `max_tokens`、`output_name` | 不能：由权重决定 | 模型（不变） |
| `vision_document` 的 `prompt`、`max_tokens` | 不能：`ocr` 类能力就是"读出全部文字"，接口 `Recognize(images)` 不带选项，识别指令是这个实现的一部分。按任务换指令属于以后的 `vlm` 类 | 模型（不变） |
| LLM 的 `system_prompt`、`random_seed` | 能 | **节点**（生成参数） |
| whisper 的 `language` | 能：同一份多语种权重，可以一个节点转中文、另一个转英文 | **节点**（AsrTranscribeNode） |
| `max_tokens`、`temperature` 等采样参数 | 能 | 节点（不变） |
| 批大小、上下文容量、线程、GPU 层数、run config | 不能：加载时分配 | 后端（批大小从模型移除） |

规则同样适用于以后新增的参数：
- 思考模式：按节点区分是合理的，以后作为 LLM 生成参数的可选项 `thinking`；
- LoRA 适配器：要加载哪些适配器是模型参数，节点选用哪一个是节点参数。

偏业务的后处理不做成模型参数，例如去掉 Markdown 标记、抽取字段、按阈值判断。这类处理不同节点可以不一样，做法是写一个节点：在节点里调用模型，再做后处理，其他节点使用它的输出。模型按批调用，节点拿到整批结果后逐条处理，单条与批量的逻辑相同。现有的 PromptGuidedLlmNode 就是这样写的。

| 处理 | 放在哪里 |
| --- | --- |
| 与权重绑定、所有使用者必须一致的处理（分词、池化、归一化、提示格式） | 模型参数 |
| 偏业务、不同节点可以不一样的处理 | 节点：调用模型后再处理 |

## 4. 配置格式

### 4.1 模型条目

| 键 | 必填 | 说明 |
| --- | --- | --- |
| `type` | 是 | 类别：`llm`、`embedding`、`rerank`、`ocr`、`asr` |
| `name` | 是 | 模型名，在 `models` 中唯一，节点用 `bind_model` 引用。同一类别可以有多个模型，例如 `bge-base-ori`、`bge-base-sft`、`qwen3-embedding-4b` 三个向量模型，分别被不同节点绑定 |
| `file` | 是 | 权重文件名，规则见 4.2 |
| `params` | 否 | 模型参数。省略表示全部使用默认值；只写与默认值不同的项 |
| `backend` | 是 | 对象，只允许两个键：`type` 为后端名（必填），`params` 为后端参数（可省略） |

- 条目只允许这五个键。
- `models` 是数组；本身可以省略，表示方案不用模型（不变）。

**用哪个模型实现**：配置中不写实现名，由类别和后端决定。在本构建已注册的模型实现中，找类别等于 `type`、且所需执行协议被该后端支持的那一个。注册审计保证每个（类别, 后端）组合最多对应一个实现（`GlobalInit` 时检查，冲突时 `Init` 失败并列出冲突的实现）。现有实现都满足：

| 类别 | 后端 | 实现 |
| --- | --- | --- |
| `embedding` | `onnxruntime` | `bge_embedding` |
| `embedding` | `kite_llm` | `generated_text_embedding` |
| `rerank` | `onnxruntime` | `bge_reranker` |
| `llm` | `llama_cpp`、`kite_llm` | `qwen_causal_lm` |
| `ocr` | `kite_llm` | `vision_document` |
| `asr` | `whisper_cpp` | `whisper_asr` |

**测试夹具**：测试程序和 mock Demo 会同时注册生产实现与测试模型。测试模型与生产实现的类别、协议往往相同，按上述规则会一个组合对应多个实现。测试模型有两个来源：
- `dev_support/inference/` 中的共享夹具：`test_biz_embedding`、`test_biz_rerank`、`test_biz_ocr`、`test_biz_asr` 运行在 `test_tensor_backend` 上，`test_biz_llm` 运行在 `test_causal_lm_backend` 上；
- 测试源码中自行注册的模型，分布在约 10 个测试文件中，例如 `FailingCreateModel`、`CountingModel`，以及 `tests/support/node_plan_fixture.h` 为每个类别注册的模型。它们大多与生产实现同为 `kTensorGraph` 或 `kTextGeneration`，一个测试程序里还常有多个同类别的测试模型。

处理方式：
- `ExecutionProtocol`（`include/engine/inference_definition.h`）新增 `kFixture`。测试后端只支持 `kFixture`，测试模型只要求 `kFixture`；
- `ModelDefinition` 新增 `fixture_backends`：测试模型列出自己能运行在哪些测试后端上（例如 `CountingModel` 要跑在 4 个测试后端上）。按类别和后端选实现时，`kFixture` 模型只与它列出的后端匹配。生产实现不写这个字段；
- 测试源码中自行注册的模型都改为这种写法，各自配一个测试后端。多数已经有自己的后端（如 `counting_backend`、`node_fixture_backend_<类别>`）；原来借用 `test_causal_lm_backend` 的（如 `TranslationProbeModel`）改用自己的后端。

这样每个（类别, 后端）仍只对应一个实现，同一测试程序里也可以有多个同类别的测试模型。没有其他副作用：
- 测试模型本来就不用会话做计算，只返回预设结果；
- `configs/`、`demo/fixtures/mock/` 中的方案从不把生产模型配到测试后端上。测试中有个别用例这样写（如 `test_adapter_contract_security.cpp` 把 `qwen_causal_lm` 配到 `test_causal_lm_backend`），迁移时改用对应的测试模型；
- 单元测试中直接按实现名创建模型的写法不变。

不经过 `GlobalInit` 的测试程序不做注册审计，所以 Core 自己也检查：按类别和后端选出多个实现时报 `REGISTRY_CONFLICT`（已有诊断码），列出这些实现。

同一（类别, 后端）下不同模型的差异用模型参数表达，例如以后在 onnxruntime 上接入 BPE 分词的向量模型时，给向量实现加 `tokenizer_type`（默认 `wordpiece`，现有方案不受影响）。kite 已经在内部封装了分词，这类参数只在需要框架自己做预处理的后端上才有。

按"扩展只靠新增"核对，各扩展场景的改动如下：

| 扩展场景 | 改动 |
| --- | --- |
| 在 kite 或 llama.cpp 上换同类新模型（新权重、新微调） | 只改配置 |
| 新增一个后端（例如内网 NPU），支持已有的执行协议 | 新增后端；模型实现不用改 |
| 新增一个类别，或在某个（类别, 后端）上首次提供实现 | 新增实现 |
| 在 onnxruntime 上接入 BPE 分词的向量模型 | 修改已有的向量实现，增加 `tokenizer_type`。分词器做成按名字登记的组件后，再加分词器只需新增 |
| 同一（类别, 后端）需要完全不同的处理流程 | 只能并入已有实现。这是唯一需要修改已有实现的情况，目前没有这种需求 |

类别与模型接口一一对应，由 `include/engine/model_type_traits.h` 按接口推导。可用的类别就是本构建中已注册实现的类别，与 Catalog 一致，不另外维护列表（AGENTS.md：注册与 Definition 才是可执行的 Catalog）。现有类别：

| 类别 | 输入 → 输出 | 接口 | 现有实现 | 使用它的节点 |
| --- | --- | --- | --- | --- |
| `llm` | 文本 → 文本 | `ILlmModel` | `qwen_causal_lm` | LlmGenerateNode、PromptGuidedLlmNode（节点设计中改为 `llm_generate`、`prompt_guided_llm`） |
| `embedding` | 文本 → 向量 | `IEmbeddingModel` | `bge_embedding`、`generated_text_embedding` | TextEmbeddingNode |
| `rerank` | 查询与候选 → 分数 | `IRerankModel` | `bge_reranker` | TextRerankNode |
| `ocr` | 图像 → 文字与文字框 | `IOcrModel` | `vision_document` | OcrDetectNode |
| `asr` | 音频 → 文本 | `IAsrModel` | `whisper_asr` | AsrTranscribeNode |

LLM 由框架分词、逐 token 生成，还是由后端整段生成，都属于 `llm`，差别只在所选后端。新增类别（例如"图像加提问 → 回答"的 `vlm`）时，新增接口、类别特化和对应节点即可，现有配置不受影响。

### 4.2 文件路径

| 项目 | 规则 |
| --- | --- |
| 哪些是文件 | 条目的 `file`，以及声明为文件的参数（目前是 `bge_embedding`、`bge_reranker` 的 `tokenizer_file` 和 kite_llm 的 `run_config_file`） |
| 基准 | Pipeline JSON 所在目录。外部约定：JSON 所在目录拼接文件名即可读取。`.conf` 中的 `pipe_path` 也以 `.conf` 所在目录为基准，两者一致 |
| 写法 | 文件名，可以带子目录。不允许空值、绝对路径（含盘符、UNC）和任何 `..` 分量 |
| 越界 | 解析后（含符号链接）必须仍在该目录内 |
| 解析位置 | 接入层，在 Core 校验之前。模型、后端拿到的是绝对路径，自己不处理路径 |
| 文件是否存在 | 由打开它的模型或后端在 Create 时检查；预检不要求文件已部署（mock 后端不读文件） |
| 不知道 JSON 位置时 | `validate --stdin`、Studio 草稿：只检查写法 |

`.conf` 与 `pipe_path` 的解析不变；Create 参数 `model_path`（部署根）仍只用于定位 `.conf`。

### 4.3 诊断

| 情况 | 诊断码 | 路径 |
| --- | --- | --- |
| 条目缺键、类型错误、未知键 | `MISSING_FIELD`、`FIELD_TYPE`、`UNKNOWN_FIELD`（不变） | `/models/<i>/<键>`、`/models/<i>/backend/<键>` |
| `type` 不是本构建中任何已注册实现的类别 | `UNKNOWN_MODEL_TYPE`（含义改为类别），附可用类别 | `/models/<i>/type` |
| 后端未注册 | `UNKNOWN_BACKEND`（不变），建议该类别可用的后端 | `/models/<i>/backend/type` |
| 该类别没有能在这个后端上运行的实现 | `BACKEND_PROTOCOL_MISMATCH`（不变），附该类别可用的后端 | `/models/<i>/backend/type` |
| 按类别和后端选出多个实现（只会出现在测试程序中，4.1） | `REGISTRY_CONFLICT`（已有），列出这些实现 | `/models/<i>/backend/type` |
| 参数未知、缺少、类型错误、超出范围、不在枚举内 | 与节点、converter 相同：`UNKNOWN_CONFIG_FIELD`、`MISSING_CONFIG_FIELD`、`CONFIG_FIELD_TYPE`、`CONFIG_FIELD_RANGE`、`CONFIG_FIELD_ENUM` | `/models/<i>/params/<参数>`、`/models/<i>/backend/params/<参数>` |
| 参数的 `Validate` 失败 | `INVALID_COMBINATION`（不变） | `/models/<i>/params`、`/models/<i>/backend/params` |
| 文件写法错误或越界 | `INVALID_FILE_PATH`（接入层，原 `INVALID_MODEL_PATH`） | `/models/<i>/file` 或该参数的路径 |
| `name` 重复 | `DUPLICATE_MODEL_NAME`（原 `DUPLICATE_MODEL_ID`） | `/models/<i>/name` |
| 节点的 `bind_model` 指向不存在的模型 | `UNKNOWN_MODEL_REFERENCE`（不变） | 该节点的 `bind_model` 参数 |
| 节点需要的类别与所绑模型不同 | `MODEL_TYPE_MISMATCH`（原 `MODEL_CAPABILITY_MISMATCH`） | 同上 |
| 模型没有被任何节点绑定 | `UNUSED_MODEL`（新增）。存在未知节点类型时不报，以免同一根因重复报错 | `/models/<i>/name` |
| Create 时文件不存在、加载失败、需要从模型读取的参数读不到 | `MODEL_MATERIALIZATION_FAILED`（不变），消息写明文件或参数名 | `/models/<i>` |
| 节点要求、所绑模型做不到（例如不支持的转写语言） | `NODE_INIT_FAILED`（不变） | 该节点 |

- 删除的诊断码：`UNKNOWN_MODEL_CONFIG_FIELD`、`UNKNOWN_BACKEND_CONFIG_FIELD`，并入通用参数诊断码。
- 节点侧的诊断路径随节点条目的写法变化，见[节点设计](PIPELINE_NODE_DESIGN.md) 3.3。
- 修复原因：`model_capability_mismatch` 改为 `model_type_mismatch`；`unknown_model_type` 的候选改为类别列表；`unknown_backend` 的候选改为该类别可用的后端；新增 `backend_protocol_mismatch`（现在这个诊断没有修复建议），候选同为该类别可用的后端。

### 4.4 示例

```json
// kite：线程、上下文等设置在 kite 自己的 run config 里，这里只写文件名
{
  "type": "llm",
  "name": "llm_model",
  "file": "qwen2.5-0.5b-instruct-q4_k_m.gguf",
  "backend": { "type": "kite_llm", "params": { "run_config_file": "kite_text_run.json" } }
}

// 生成式向量：kite 固定版本不能查询维度，embedding_dim 必须写
{
  "type": "embedding",
  "name": "embed_model",
  "file": "qwen2.5-0.5b-instruct-q4_k_m.gguf",
  "params": {
    "embedding_dim": 896,
    "prompt_prefix": "<|im_start|>user\n用一个词概括以下文本：\n",
    "prompt_suffix": "<|im_end|>\n<|im_start|>assistant\n" },
  "backend": { "type": "kite_llm", "params": { "run_config_file": "kite_text_run.json" } }
}

// 同一类别的多个模型，分别被不同节点绑定
{ "type": "embedding", "name": "bge-base-ori", "file": "bge_base_zh_v1.5.onnx",
  "params": { "tokenizer_file": "bge_base_zh_v1.5_vocab.txt" },
  "backend": { "type": "onnxruntime" } },
{ "type": "embedding", "name": "bge-base-sft", "file": "bge_base_sft.onnx",
  "params": { "tokenizer_file": "bge_base_zh_v1.5_vocab.txt" },
  "backend": { "type": "onnxruntime" } }
```

翻译方案：系统提示词和种子从模型移到节点。

```json
// 模型条目
{ "type": "llm", "name": "translate_llm", "file": "qwen2.5-0.5b-instruct-q4_k_m.gguf",
  "backend": { "type": "llama_cpp", "params": { "n_threads": 4, "n_threads_batch": 4 } } }

// 绑定它的 LLM 节点的参数（节点条目的写法见节点设计）
{ "bind_model": "translate_llm", "system_prompt": "你是专业翻译。……",
  "random_seed": 0, "max_tokens": 512, "temperature": 0.0 }
```

语音：转写语言是节点参数，默认 `zh` 时不用写。

```json
// 模型条目
{ "type": "asr", "name": "asr_model", "file": "ggml-base.bin", "backend": { "type": "whisper_cpp" } }

// AsrTranscribeNode 的参数
{ "bind_model": "asr_model" }
```

### 4.5 现有配置的迁移规则

在本地一次性迁移；迁移脚本不提交（CONTRIBUTING §3）。

1. 条目：
   - `model_id` 改为 `name`，按用途命名、去掉后端名：`embed_model_onnx` 改为 `embed_model`，`rerank_model_onnx` 改为 `rerank_model`，`llm_model_llamacpp` 改为 `llm_model`；同步修改节点的 `bind_model`；
   - 原 `model_type`（实现名）删除，改写实现所属的类别 `type`；
   - `model_path` 改为 `file`，只保留文件名（去掉 `./models/`、`models/`）；
   - `model_config` 改为 `params`，为空时省略；
   - `backend` 与 `backend_config` 合成 `backend: {"type": 后端名, "params": 参数}`，参数为空时省略 `params`；
   - 键的顺序：`type`、`name`、`file`、`params`、`backend`。
2. 参数：
   - 按 6.2 改名；
   - 删除 `bge_embedding`、`bge_reranker` 的 `max_batch_size`。现有值都是 4，等于 onnxruntime 的默认值，不用搬到后端；
   - `qwen_causal_lm` 的 `system_prompt`、`random_seed` 不等于默认值时，写到绑定该模型的 LLM 节点。现有两处：`translate_cpu` 的 `system_prompt` 和 `random_seed: 0`，`dialogue_audit_kite` 的 `system_prompt`（写到 `node_7_LlmGenerateNode`）；
   - 方案中的向量节点都没有写 `normalize`，模型上也不用写（默认值相同）；
   - whisper 的 `language` 不等于 `zh` 时写到 AsrTranscribeNode。现有方案都是 `zh`，直接删除；
   - 等于默认值的参数一律删除，这是 [业务开发者体验 RFC](DEVELOPER_EXPERIENCE_RFC.md) 待决事项 O-1 的决定。`configs/` 中的清单见附录 A，`demo/fixtures/mock/` 按同一规则处理；测试夹具按测试需要决定；
   - `bge_embedding.embedding_dim`：实施时用真实权重确认 `bge_base_zh_v1.5.onnx` 的输出维度是固定值，确认后删除；不是固定值则保留。
3. 测试用方案：测试模型上的 `max_batch_size: N` 改为测试后端的 `fixed_batch_size: N`（第 7 节），保持原有的固定批行为。`demo/fixtures/mock/` 中引用夹具文件的两个实体抽取方案，`file` 改为相对方案目录的 `artifacts/neutral-llm.fixture`。
4. 文件：权重、词表、kite run config 与方案放在同一目录（第 12 节）。

需要迁移的文件：
- `configs/*.json`（17 个，含 kite 方案）；
- `demo/fixtures/mock/*.json`（9 个）；
- `tests/fixtures/pipelines/` 下的 JSON（含 `validation/invalid_pipeline_cases.json`）；
- 资源清单 `configs/asset_manifest.json`（原 `models/asset_manifest.json`）与 `tests/fixtures/asset_manifest_test.json` 中的选型模板（13.4）；
- C++、Python、JS 测试中内嵌的配置文本；
- 文档里的配置示例。

## 5. 参数机制

### 5.1 统一的四段写法

节点、converter、模型、后端写参数都是同样的四段；改参数只动前两段。以 `bge_embedding` 为例，下面是改造完成后的样子；阶段 1 只换写法，参数名、`tokenizer_file` 的默认值和文件解析方式要到阶段 2 才变（第 14 节）：

```cpp
// src/engine/models/bge_embedding/bge_embedding_model.cpp
namespace {
// ① 参数结构体
struct Params {
  std::string tokenizer_file;            // Create 时已是绝对路径
  std::optional<int64_t> embedding_dim;  // 不写时从模型读取
  std::optional<int64_t> max_tokens;     // 不写时从模型读取，读不到时为 512
  bool do_lower_case = true;
  std::string pooling;
  bool normalize = true;
  std::string output_name;
};

// ② 参数声明：名字、默认值、范围、说明只写在这里
Parameters<Params> ParamSpec() {
  auto spec = Parameters<Params>(
      {Field("tokenizer_file", &Params::tokenizer_file).Required().File()
           .Description("与权重匹配的 BERT WordPiece 词表文件"),
       Field("embedding_dim", &Params::embedding_dim).Range(1, 65536)
           .Description("输出向量维数；不写时从模型输出张量读取，读不到时必须填写"),
       Field("max_tokens", &Params::max_tokens).Range(2, 4096)
           .Description("每条文本编码后的 token 数，含 [CLS]/[SEP]，超长截断、不足补齐；"
                        "不写时从模型输入张量读取，读不到时为 512"),
       Field("do_lower_case", &Params::do_lower_case).Default(true)
           .Description("分词前把英文字母归一为小写，须与训练时一致"),
       Field("pooling", &Params::pooling).Default("cls").Enum({"cls", "mean"})
           .Description("三维输出的池化方式：cls 取首个 token，mean 按 attention_mask 求均值"),
       Field("normalize", &Params::normalize).Default(true)
           .Description("把输出向量缩放为长度 1；绑定同一模型的节点结果一致"),
       Field("output_name", &Params::output_name).Default("last_hidden_state")
           .Description("读取的输出张量名，须与导出时一致")});
  spec.Validate([](const Params& p, std::string* error) {
    if (!p.output_name.empty()) return true;
    if (error) *error = "output_name 不能为空";
    return false;
  });
  return spec;
}
}  // namespace

// ③ 登记：Definition 里只多这一行
def.params = ParamSpec();

// ④ 取用：已校验、已补默认值、文件已解析
std::shared_ptr<IModel> BgeEmbeddingModel::Create(const ModelCreateContext& ctx,
                                                  std::string* diagnostic) {
  const auto& p = ctx.Params<Params>();
  ...
}
```

后端相同。例如 onnxruntime 在 `Load` 里写 `const auto& p = spec.Params<Params>();`，再使用 `p.intra_op_num_threads`。

| | 登记 | 取用 |
| --- | --- | --- |
| 节点 | `MakeNodeSpec(..., ParamSpec(), ...)` | `Run(inputs, params, ...)` |
| converter | `def.params = ParamSpec();` | `options.Params<Params>()` |
| 模型 | `def.params = ParamSpec();` | `ctx.Params<Params>()` |
| 后端 | `def.params = ParamSpec();` | `spec.Params<Params>()` |

约束：
- 模型、后端只能读自己名下的参数。模型需要后端的情况（例如上下文容量）时，通过会话接口查询，不读后端参数。
- `Create` / `Load` 只做依赖已加载内容的检查：会话接口类型、批策略、张量形状、文件能否打开。范围、枚举、必填、跨字段规则都由声明和 `Validate` 负责，不再复查。
- 跨字段规则写在 `Validate` 里，例如 llama_cpp 的 `decode_batch_size` 不大于 `context_size`，`vision_document` 的 `prompt` 非空且不含 NUL。

### 5.2 通用参数容器

[Pipeline I/O 设计](PIPELINE_IO_DESIGN.md) 5.2 节为 converter 定义的类型擦除容器，改为放在 `contracts/` 并使用中性名字，converter、模型、后端三处共用（该文档已同步修改）：

```cpp
// include/contracts/parameter_set.h（新增）
// Create 阶段生成，之后只读共享。
class ParameterValues {
 public:
  template <typename P>
  const P& Get() const;  // 类型与声明不符时抛出 std::logic_error
  std::optional<int64_t> Integer(const std::string& name) const;  // 按名字读整数参数（converter 的尺寸参数用）
  const nlohmann::json& Effective() const;  // 生效值，供工具报告
};

class ParameterSet {
 public:
  ParameterSet() = default;  // 没有参数：配置只能是 {}
  template <typename P>
  ParameterSet(Parameters<P> spec);  // NOLINT：允许 def.params = ParamSpec();
  const std::vector<ConfigFieldDefinition>& Fields() const;
  // 依次执行：按 Fields() 校验并补齐默认值、字段赋值、Prepare、Validate、生成生效值。
  // config 可以是原始配置，也可以是已归一化的配置。
  bool Parse(const nlohmann::json& config,
             std::shared_ptr<const ParameterValues>* values,
             std::string* error) const noexcept;
};
```

Core 预检时，先按 `Fields()` 逐字段校验（得到精确的诊断路径），再调用 `Parse` 执行 `Validate`；运行时由工厂直接调用 `Parse`。两处调用的是同一个函数。

### 5.3 可选参数与从模型读取

**可选参数**：成员类型为 `std::optional<T>`，表示"可以不写"。
- `FieldTypeTraits` 新增 `std::optional<T>` 特化（`T` 为已支持的标量类型），类型、范围、枚举规则与 `T` 相同。
- 可选参数不能声明 `Required()` 或 `Default()`，不写时为空；其他参数仍须二选一。这两条由 `FieldBuilder::Build` 检查。
- Catalog 中显示为非必填、无默认值；说明里写明不写时的行为。

**从模型读取**：只在有真实接口时使用。规则写在一个公共函数里：

```cpp
// src/engine/models/common/from_model.h（新增）
// 取"可从模型读取"的参数的最终值：
// 配置写了：用配置值；模型也给出且两者不同时报错。
// 配置没写：用模型给出的值；模型给不出时用 fallback；没有 fallback 时报错，提示在参数中填写。
// 最终值写入 Create 日志（ALG_LOG_INFO）。
bool ResolveFromModel(const char* name, std::optional<int64_t> configured,
                      std::optional<int64_t> detected, std::optional<int64_t> fallback,
                      int64_t* value, std::string* diagnostic);
```

| 参数 | 从哪里读 | 读不到时 |
| --- | --- | --- |
| `bge_embedding.embedding_dim` | ONNX 输出张量 `output_name` 的最后一维（固定值时） | 必须填写 |
| `bge_embedding.max_tokens`、`bge_reranker.max_tokens` | ONNX 输入张量的序列维（固定值时） | 512 |

`bge_common/bert_model_support.*` 新增两个辅助函数 `StaticOutputDim(session, output_name)` 和 `StaticSequenceLength(session)`：维度为固定正数时返回该值，否则返回空。现有的形状核对保留，核对对象改为最终值。

不自动读取的参数：
- `generated_text_embedding.embedding_dim`：仓库固定版本的 kiteLLM 头文件没有查询模型维度的接口，维度只能在一次生成之后从任务输出取得（`kiteLLM_TaskOutput_GetOutputEmbedding`），所以仍然必填。换成有查询接口的版本后再改为自动读取，配置写法不变。
- `vision_document.patch_size`：kite 没有查询接口。
- `context_size`、线程数：是资源选择，不是模型事实。

`resolve-conf` 不加载模型，所以从模型读取的参数不出现在它的输出中。

### 5.4 文件参数

- `FieldBuilder` 新增 `.File()`，只能用于 `std::string` 或 `std::optional<std::string>` 成员（编译期检查）。
- `ConfigFieldDefinition` 新增 `bool file = false`；`ValidateConfigFieldDefinitions` 检查 `file` 只出现在字符串参数上；Catalog 中这类参数带 `"file": true`。
- 可选的文件参数用 `std::optional<std::string>`，不写表示不使用（kite 的 `run_config_file`）。不再有"空字符串表示不用"的约定，空字符串按写法错误报错。
- 路径规则只实现一次：

```cpp
// include/contracts/path_utils.h（新增函数）
// 解析写在某个文件里的相对文件名，基准为该文件所在目录（4.2）。
// 拒绝空值、绝对路径（含盘符、UNC）和任何 ".." 分量；规范化（含符号链接）后必须仍在 base_dir 内。
// 不检查文件是否存在。base_dir 为空时只检查写法。
bool ResolveFileUnderDirectory(const std::filesystem::path& base_dir, const std::string& value,
                               std::filesystem::path* resolved, std::string* error);
```

- 接入层的 `ResolveModelFiles`（取代 `ResolveDeploymentModelPaths`）对每个模型条目：
  1. 解析 `file`；
  2. 按类别和后端选出模型实现（4.1），取实现和后端的 Definition，对其中声明为文件、且在参数里出现的字符串逐个解析；
  3. 写回绝对路径。结构不对或选不出实现的条目跳过，由 Core 报告。
- kite_llm 读入 run config 后，其中的 `vision.mmproj` 也用同一个函数检查，基准为 run config 所在目录。run config 由 kite 自己读取，我们只按同一条规则检查其中的路径。

### 5.5 Definition、创建上下文与工厂

```cpp
// include/engine/inference_definition.h
struct ModelDefinition {
  std::string impl_name;   // 实现名，如 bge_embedding；配置中不写，只用于 Catalog、日志和单元测试
  std::string model_type;  // 类别，由所实现的接口决定
  std::string description;
  ExecutionProtocol required_protocol = ExecutionProtocol::kTensorGraph;
  std::vector<std::string> fixture_backends;  // 只用于 kFixture 测试模型：能运行在哪些测试后端上（4.1）
  ParameterSet params;
  InferenceConcurrency concurrency = InferenceConcurrency::kSerialized;
};

struct BackendDefinition {
  std::string backend_type;  // 后端名，即配置中 backend.type 的值（字段名不变）
  std::string description;
  std::vector<ExecutionProtocol> supported_protocols;
  ParameterSet params;
  InferenceConcurrency concurrency = InferenceConcurrency::kSerialized;
};

// include/engine/model_registry.h
struct ModelCreateContext {
  std::shared_ptr<IBackendSession> backend_session;
  std::shared_ptr<const ParameterValues> params;  // 本模型名下的参数
  template <typename P>
  const P& Params() const { return params->Get<P>(); }
};

// include/engine/backend_interface.h
struct BackendLoadSpec {
  explicit BackendLoadSpec(ExecutionProtocol protocol) : requested_protocol(protocol) {}
  std::string model_file;  // 绝对路径
  std::shared_ptr<const ParameterValues> params;  // 本后端名下的参数
  ExecutionProtocol requested_protocol;
  ExecutionTarget execution_target;
  template <typename P>
  const P& Params() const { return params->Get<P>(); }
};

// include/engine/model_runtime_factory.h
struct ModelLoadSpec {
  std::string impl_name;     // 由 Core 按类别和后端选定
  std::string backend_type;
  std::string model_file;
  nlohmann::json model_params = nlohmann::json::object();
  nlohmann::json backend_params = nlohmann::json::object();
  ExecutionTarget execution_target;
};
```

删除：两个 Definition 的 `config_fields`、`validate_config`；`ModelCreateContext` 的 `model_resource_root`、`model_config`；`BackendLoadSpec` 的 `backend_config`。

`ModelRegistry` 新增 `FindImplementation(model_type, backend_type)`：在已注册实现中找类别相同、所需协议被该后端支持的实现（`kFixture` 模型还要求该后端在它的 `fixture_backends` 中）。返回选出的全部实现：没有时能列出该类别可用的后端，多于一个时由调用方报 `REGISTRY_CONFLICT`。`ModelRegistry::Audit(const BackendRegistry&)` 检查每个（类别, 后端）最多对应一个实现，由 `GlobalInit` 调用，与 converter 审计一起报告。

`ModelRuntimeFactory::Create` 的步骤：
1. 按实现名和后端名查找两个 Definition，检查后端支持模型要求的执行协议（不变）；
2. 两组参数各调用一次 `params.Parse`【改：删除模型参数的第二次归一化与 `validate_config`】；
3. 创建后端提供者并 `Load`（不变）；
4. 核对会话的名字、并发、协议与批策略（不变）；
5. 创建模型【改：删除 `model_resource_root` 的推导】；
6. 核对模型的 `ImplName()`、`ModelType()` 与并发（不变）。

Core 的对应结构：

```cpp
// include/core/pipeline_config.h
struct ParsedModelConfig {
  std::string model_name;  // 条目的 name
  std::string model_type;  // 条目的 type（类别）
  std::string model_file;
  nlohmann::json model_params = nlohmann::json::object();
  std::string backend_type;
  nlohmann::json backend_params = nlohmann::json::object();
  size_t source_index = 0;
};

// include/core/pipeline_validator.h
struct ValidatedModelPlan {
  std::string model_name;
  std::string model_type;
  std::string impl_name;          // 按类别和后端选出
  std::string backend_type;
  std::string model_file;         // 接入层已解析
  nlohmann::json model_params;    // 已归一化
  nlohmann::json backend_params;  // 已归一化
  ExecutionProtocol protocol = ExecutionProtocol::kTensorGraph;
  InferenceConcurrency effective_concurrency = InferenceConcurrency::kSerialized;
  size_t source_index = 0;
};
```

`ModelRegistration`（`include/core/session_context.h`）的字段同名对应：`model_type`、`impl_name`、`backend_type`、`model_file`、`model_params`、`backend_params`。`RegisterBatch` 核对 `ImplName()` 与 `ModelType()`。模型版本标识由实现名、后端名、文件和两组参数组成，向量缓存的键依赖它。

## 6. 命名

### 6.1 类别、名字与实现

规则：每个条目的 **`type` 说"是哪一类"，`name` 说"是哪一个"**，与 `io`、节点一致。对模型而言：`type` 是类别，`name` 是这个模型实例的名字，实现名不写进配置。代码字段带实体前缀，与 JSON 对应：

| JSON | 代码 |
| --- | --- |
| 条目的 `type`（类别） | `model_type`、`ModelType()` |
| 条目的 `name` | `model_name`、`ModelName()` |
| 条目的 `file` | `model_file` |
| `backend.type` | `backend_type`、`BackendType()`（不变） |
| 不出现（实现名） | `impl_name`、`kImplName`、`ImplName()` |

改名表：

| 现在 | 改为 |
| --- | --- |
| 已有的、含义不同的 `model_name`：模型调用（`include/nodes/model_calls.h`）与 `bge_common/bert_model_support.*` 中报错用的标签 | `label`（最先改，避免与下面的新含义冲突） |
| `ModelDefinition::model_type`、模型类的 `kModelType`、`IModel::ModelType()`（都指实现名） | `impl_name`、`kImplName`、`ImplName()` |
| `ModelDefinition::capability`、`IModel::Capability()`（都指类别） | `model_type`、`ModelType()` |
| `include/engine/model_capability_traits.h` 中的 `ModelCapabilityTraits<I>::Capability()` | `include/engine/model_type_traits.h` 中的 `ModelTypeTraits<I>::ModelType()` |
| 模型实例的 `model_id`：`ParsedModelConfig`、`ValidatedModelPlan`、`ModelManager`、`ModelCallBase::ModelId()`、`ResolvedNodeModelBinding`、`SessionResources::GetModelRevision` 等 | `model_name`、`ModelName()` |
| `ModelRegistry` 的 `Find/Create/Has(model_type)`、`ListTypes()`；`PipelineCatalog::FindModel` | 参数为实现名；`ListImplNames()`；新增 `FindImplementation(model_type, backend_type)` |
| `NodeModelDependency::capability`、`ResolvedNodeModelBinding::capability`、`ModelSlotBinding::Capability()` | `model_type`、`ModelType()` |
| `ModelCallBase::ModelType`（类型别名） | `Interface`，避免与 `ModelType()` 同名 |
| 诊断码 `MODEL_CAPABILITY_MISMATCH`、修复原因 `model_capability_mismatch` | `MODEL_TYPE_MISMATCH`、`model_type_mismatch` |
| 诊断码 `DUPLICATE_MODEL_ID` | `DUPLICATE_MODEL_NAME` |
| 接入层诊断 `INVALID_MODEL_PATH` | `INVALID_FILE_PATH` |
| `src/adapter/deployment_model_resolver.*`、`ResolveDeploymentModelPaths` | `src/adapter/model_file_resolver.*`、`ResolveModelFiles` |

- 替换顺序：先改已有的 `model_name` 标签，再把表示实现名的 `model_type` 改为 `impl_name`，然后把 `capability` 改为 `model_type`，最后把 `model_id` 改为 `model_name`，以免误替换。
- 后端的 `backend_type`、`kBackendType`、`BackendType()` 本来就指后端名，与 JSON 的 `backend.type` 一致，保持不变。
- `ModelIdentity<Model, 接口>` 改为要求 `kImplName`；类别仍由接口推导，作者不写。
- 节点中描述模型依赖的文字由框架按类别生成（节点设计 5.4），本阶段改为"引用 `models[].name`；所选模型的类别必须是 llm"。
- 节点条目的 `type`、`name` 见[节点设计](PIPELINE_NODE_DESIGN.md) 第 3 节。

### 6.2 参数命名

1. 使用 snake_case；同一个概念在全项目用同一个名字。
2. 上限参数以单位结尾：`max_tokens`、`max_pixels`、`max_audio_seconds`、`max_output_bytes`。io 的 `answer_text_max_bytes` 也遵循这条。
3. 文件参数以 `_file` 结尾，并声明 `.File()`。
4. 后端参数沿用各推理库自己的术语（`n_threads`、`intra_op_num_threads`、`n_gpu_layers`），方便对照官方文档，不跨后端统一。
5. 参数说明用中文。

| 现在 | 改为 | 依据 |
| --- | --- | --- |
| 条目的 `model_path` | `file` | 条目中已有 `type`、`name`，不再加前缀；也避免与 Create 参数 `model_path`（部署根目录）同名不同义 |
| `bge_embedding`、`bge_reranker` 的 `max_length` | `max_tokens` | 规则 2 |
| `bge_embedding` 的 `pooling_strategy` | `pooling` | 规则 1，与 `generated_text_embedding` 一致 |
| `generated_text_embedding` 的 `prefix`、`suffix` | `prompt_prefix`、`prompt_suffix` | 名字说明是提示词的前缀、后缀 |

## 7. 改造后的参数

"自动"表示可选参数，不写时从模型读取（5.3）；"文件"按 4.2 解析。

**模型**

| 实现（类别） | 参数（默认值，范围） |
| --- | --- |
| `bge_embedding`（embedding） | `tokenizer_file` 文件，必填；`embedding_dim` 自动，读不到时必填（1–65536）；`max_tokens` 自动，读不到时为 512（2–4096）；`do_lower_case` true；`pooling` cls（cls / mean）；`normalize` true；`output_name` last_hidden_state（非空） |
| `bge_reranker`（rerank） | `tokenizer_file` 文件，必填；`max_tokens` 自动，读不到时为 512（3–4096）；`do_lower_case` true；`output_name` logits（非空）；`score_activation` sigmoid（sigmoid / identity） |
| `generated_text_embedding`（embedding） | `embedding_dim` 必填（1–65536）；`max_tokens` 1（1–64）；`pooling` last（last / mean）；`normalize` true；`prompt_prefix` ""；`prompt_suffix` ""；`add_bos` false |
| `qwen_causal_lm`（llm） | `add_bos` false |
| `vision_document`（ocr） | `prompt` 默认识别指令（非空、不含 NUL）；`patch_size` 16（1–256）；`max_pixels` 4194304（1–16777216）；`max_tokens` 512（1–4096） |
| `whisper_asr`（asr） | `max_audio_seconds` 30（1–60）；`max_output_bytes` 65536（1–65536） |

**后端**

| 后端 | 参数（默认值，范围） |
| --- | --- |
| `onnxruntime` | `max_batch_size` 4（1–1024）；`intra_op_num_threads` 2（1–64）；`inter_op_num_threads` 1（1–64）；`graph_optimization_level` all（none / basic / extended / all） |
| `llama_cpp` | `context_size` 2048（16–1048576）；`decode_batch_size` 512（1–1048576，不大于 `context_size`）；`n_threads` 0（0–1024，0 表示 llama.cpp 默认值）；`n_threads_batch` 0（同上）；`n_gpu_layers` 0（0–1048576）；`check_tensors` false |
| `kite_llm` | `run_config_file` 文件，可选，不写表示不用 run config。线程、上下文等在 kite 自己的 run config 中设置，设备号来自 Create 参数 |
| `whisper_cpp` | `n_threads` 4（1–64） |

**测试用（`dev_support/inference/`）**
- `test_tensor_backend`、`test_causal_lm_backend` 新增 `fixed_batch_size`，默认 0（0–16）：为 0 时会话的批策略与现在相同；大于 0 时会话报告该固定批。
- 测试后端只支持 `kFixture` 协议；测试模型只要求 `kFixture`，并用 `fixture_backends` 列出所用的测试后端（4.1）。
- 测试模型删除 `max_batch_size`，改用会话的批策略；`test_biz_embedding` 的 `embedding_dim`（384）和 `test_biz_llm` 的 `max_seq_len`（512）保留。

**节点新增参数**

| 节点 | 参数（默认值） | 说明 |
| --- | --- | --- |
| LlmGenerateNode、PromptGuidedLlmNode（生成参数） | `system_prompt` "" | ChatML 的 system 角色内容；为空时省略该角色 |
| 同上 | `random_seed` -1（-1 到 2147483647） | -1 表示不固定；非负值结合 req_id、sub_id 派生每条输入的采样种子 |
| AsrTranscribeNode | `language` "zh"（非空） | 转写语言，例如 zh、en；auto 为自动识别。Create 时由所绑模型确认支持（whisper 的 `zh` 需要多语种权重） |

**节点删除的参数**

| 节点 | 参数 | 去向 |
| --- | --- | --- |
| TextEmbeddingNode | `normalize` | 移到向量模型（8.4） |
| PromptGuidedLlmNode | `prompt_prefix` | 删除：它只是在模板前加一行文字，写进 `prompt_template` 开头效果相同；system 角色用 `system_prompt` |

## 8. 节点侧改动

### 8.1 生成参数

```cpp
// include/contracts/inference_payloads.h
struct GenerateOptions {
  std::string system_prompt;  // 新增
  int max_tokens = 128;
  float temperature = 0.7f;
  int top_k = 0;
  float top_p = 0.9f;
  float repetition_penalty = 1.0f;
  std::vector<std::string> stop_words;
  int64_t random_seed = -1;   // 新增
};
```

- 两个 LLM 节点共用的生成参数声明加入这两项。[节点设计](PIPELINE_NODE_DESIGN.md)阶段 1 先合入，已把这组声明改为 `Field` 写法（其 5.3），这里只需在 `GenerateParameters()` 中加两行 `Field`，默认值取自 `GenerateOptions`。
- `QwenCausalLmModel` 从 `options.system_prompt` 生成 ChatML 的 system 段，从 `options.random_seed` 派生种子；删除同名成员与构造参数。
- PromptGuidedLlmNode 删除 `prompt_prefix`（第 7 节）；`src/custom_nodes/README.md` 同步修改。

### 8.2 转写语言

```cpp
// include/contracts/inference_payloads.h
struct TranscribeOptions {
  std::string language = "zh";
};

// include/engine/model_interface.h
class IAsrModel : public IModel {
 public:
  virtual bool SupportsLanguage(std::string_view language) const noexcept = 0;
  virtual int Transcribe(const AudioPcmBatch& audio, const TranscribeOptions& options,
                         TextBatch* outputs, std::string* diagnostic = nullptr) noexcept = 0;
};
```

- `AsrCall`（`include/nodes/model_calls.h`）的 `Transcribe` 增加 `options` 参数，并新增 `SupportsLanguage(language)`。
- `WhisperAsrModel`：每次调用按 `options.language` 设置会话选项；`SupportsLanguage` 转给会话；删除 Create 时的语言检查和 `language` 参数。测试用的 `TestBizAsrModel::SupportsLanguage` 返回 true。
- AsrTranscribeNode 不用枚举限定语言：支持哪些语言由所绑模型决定，以后接入别的 ASR 模型时不必改这个公共节点。

```cpp
// src/common_nodes/asr_transcribe_node.cpp
NodeResult<TextBatch> Run(const Inputs& inputs, const TranscribeOptions& params,
                          const Models& models) {
  return models.transcriber.Transcribe(*inputs.audio, params);
}

auto Spec() {
  auto params = Parameters<TranscribeOptions>(
      {Field("language", &TranscribeOptions::language).Default("zh")
           .Description("转写语言，例如 zh、en；auto 为自动识别。所绑模型须支持该语言")});
  params.Validate([](const TranscribeOptions& p, std::string* error) {
    if (!p.language.empty()) return true;
    if (error) *error = "language 不能为空";
    return false;
  });
  return MakeNodeSpec(InputsOf<Inputs>{Required("audio", &Inputs::audio)},
                      PreservedOutput<TextBatch>("text", "audio"), std::move(params),
                      ModelsOf<Models>{Model("transcriber", "bind_model", &Models::transcriber)},
                      &Run)
      .ValidateModels([](const TranscribeOptions& p, const Models& m, std::string* error) {
        if (m.transcriber.SupportsLanguage(p.language)) return true;
        if (error) *error = "所绑定的 asr 模型不支持语言: " + p.language;
        return false;
      })
      .Category("common")
      .ParallelSafe(true)
      .Description("Audio speech recognition (ASR) transcription node");
}
```

### 8.3 节点在 Create 阶段核对所绑模型

`NodeSpec` 新增可选的 `ValidateModels(fn)`，`fn` 的签名为 `bool(const Params&, const Models&, std::string*)`。

- `AuthorNode::InitNode` 在绑定模型之后调用它；返回 false 时节点初始化失败，Create 报 `NODE_INIT_FAILED`。
- 只在 Create 阶段执行一次。
- 第 3 节"节点要求、所绑模型做不到时在 Create 报错"由它实现；以后的思考模式也用它。

### 8.4 向量归一化移到模型

- `bge_embedding`、`generated_text_embedding` 新增参数 `normalize`（默认 true，第 7 节），在 `Create` 时读入，`Embed` 时使用。
- `include/contracts/inference_payloads.h` 删除 `EmbeddingOptions`（只有 `normalize` 一个字段）；`IEmbeddingModel::Embed`、`EmbeddingCall::Embed` 去掉选项参数。
- TextEmbeddingNode 删除 `normalize` 参数。会话缓存的键不再单独带 `normalize`：它随模型参数变化，键里的模型版本标识（5.5）已经包含模型参数。
- 方案中的向量节点都没有写 `normalize`，迁移时不用搬。

## 9. 运行链路与校验顺序

```text
Create（每个句柄一次）
  1. .conf → pipe_path → Pipeline JSON（不变）
  2. 拆出 io（见 I/O 设计）
  3. 解析模型文件：file 与文件参数，相对 Pipeline JSON 所在目录                         【改】
  4. Core 校验 models：结构 → 类别 → 后端 → 按类别和后端选出实现 → 参数                  【改】
  5. Core 校验节点的 bind_model：模型存在、类别一致；最后检查没有被使用的模型            【改】
  6. 物化模型：工厂各解析两组参数一次 → 后端 Load → 模型 Create
     （读参数、从模型读取能读的事实、只做依赖会话的检查）                                  【改】
  7. 创建节点：解析节点参数 → 绑定模型 → ValidateModels                                   【新】
Process（每批一次）
  节点调用模型：生成参数带 system_prompt、random_seed；转写带 language                   【改】
```

Core 对每个模型条目的校验顺序（`src/core/pipeline_validator.cpp`）：

1. `type` 是否为本构建中已注册实现的类别；
2. 按名字查找后端；找不到时，建议该类别可用的后端；
3. `FindImplementation(type, backend.type)` 选出实现；选不出时报 `BACKEND_PROTOCOL_MISMATCH`，附该类别可用的后端；选出多个时报 `REGISTRY_CONFLICT`；
4. 两组参数：先按声明逐字段校验（路径分别为 `/models/<i>/params`、`/models/<i>/backend/params`），再调用 `Parse` 执行 `Validate`；
5. 记录条目的 `type`，供节点绑定检查使用。选不出实现时也记录，以免同一根因重复报错。

所有节点检查完之后，没有被任何节点绑定的模型报 `UNUSED_MODEL`。

另外两处变化：
- Core 不再检查文件路径，路径只由接入层处理（4.2）；
- `ValidateAndNormalizeConfig` 删除 `unknown_field_code` 参数。

## 10. 与现行指南、旧计划的关系

1. 开发指南写的"模型能力来自 `model_type` 对应的注册 Definition，不在 JSON 中重复声明"：改为在条目中写类别 `type`，不写实现名，实现由类别和后端决定。理由：同一类别可以有多个模型，配置要能一眼看出每个模型属于哪一类。这是用户的决定。
2. 开发指南与 Model Execution 参考写的"`validate_config` 由校验器和工厂调用，`Create` 复用相同检查"：改为参数只在 `ParameterSet::Parse` 中校验一次，`Create` 不复查。
3. 开发指南中"BGE 不再接受 `model_config.normalize`，已有配置应移到节点"：与新规则相反（`normalize` 回到模型参数，8.4），与 M12 的代码一起删除。
4. [FRAMEWORK_SIMPLIFICATION_PLAN.md](FRAMEWORK_SIMPLIFICATION_PLAN.md) 第 10 节有两行被本设计取代，阶段 2 中同步改写：
   - "合并 `model_config.max_batch_size` 与 `backend_config.max_batch_size`：不做（删掉会让现有配置失效）"：项目尚未发布、不做兼容，按 M5 只保留后端的批大小；
   - "批量删除配置中等于默认值的显式字段：不做，交给方案负责人"：按 RFC 待决事项 O-1 的决定执行：等于默认值的一律删除，`configs/` 中的清单见附录 A。

## 11. 删除清单（遗留与冗余）

| 位置 | 删除 | 阶段 |
| --- | --- | --- |
| `include/contracts/config_schema.h` | `ConfigValueOrDefault` | 1 |
| `include/engine/inference_definition.h` | 两个 Definition 的 `config_fields`、`validate_config` | 1 |
| `src/engine/runtime/model_runtime_factory.cpp` | 模型参数的第二次归一化与 `validate_config` 调用 | 1 |
| `src/engine/models/bge_common/bert_model_support.*` | `ValidateBertModelConfig`，改为声明与 `Validate` | 1 |
| `src/engine/models/bge_embedding/` | 拒绝 `normalize` 的分支（阶段 2 中 `normalize` 改为正式参数，8.4）；`embedding_dim` 的存在性复查 | 1 |
| `src/engine/models/bge_reranker/` | `max_length`、`score_activation`、`max_batch_size` 复查 | 1 |
| `src/engine/models/generated_text_embedding/` | 维数、token 上限、`pooling` 复查 | 1 |
| `src/engine/models/qwen_causal_lm/` | `random_seed` 复查 | 1 |
| `src/engine/models/vision_document/` | `Create` 对 `ValidateVisionConfig` 的重复调用，以及 `patch_size`、`max_pixels` 复查 | 1 |
| `src/engine/models/whisper_asr/` | `language`、`max_audio_seconds`、`max_output_bytes` 复查 | 1 |
| 各模型 `Create` 中对会话协议的再次比较 | 工厂已核对；接口类型转换和批策略检查保留 | 1 |
| `src/engine/backends/llama_cpp/` | `LlamaCppConfigFields`、`ParseLlamaCppConfig` 和重复的默认值常量 | 1 |
| `src/engine/backends/kite_llm/`、`src/engine/backends/whisper_cpp/` | 手写的未知字段循环与范围检查 | 1 |
| `dev_support/inference/test_biz_models.cpp` | `ConfigSize` 等手写取值 | 1 |
| `include/core/diagnostic_code.h` | `UNKNOWN_MODEL_CONFIG_FIELD`、`UNKNOWN_BACKEND_CONFIG_FIELD` | 2 |
| `include/contracts/inference_payloads.h`、`include/nodes/model_calls.h`、`include/engine/model_interface.h` | `EmbeddingOptions` 及 `Embed` 的选项参数（8.4） | 2 |
| `src/common_nodes/text_embedding_node.cpp` | `normalize` 参数及缓存键中的 `normalize` | 2 |
| `src/custom_nodes/prompt_guided_llm_node.cpp` | `prompt_prefix` 参数 | 2 |
| `src/core/pipeline_validator.cpp` | `model_path` 的词法检查；`ValidateAndNormalizeConfig` 的 `unknown_field_code` 参数 | 2 |
| `include/engine/model_registry.h`、`src/engine/runtime/model_runtime_factory.cpp` | `model_resource_root` 及其推导 | 2 |
| `src/engine/models/bge_common/bert_model_support.*` | `ResolveTokenizerResourcePath`；`ValidateModelBatchLimit`、`ConstrainModelBatchPolicy`（批策略只来自会话） | 2 |
| `src/engine/backends/kite_llm/kite_llm_backend.cpp` | `ResolveRunConfig`，改用 `ResolveFileUnderDirectory` | 2 |
| `src/adapter/deployment_model_resolver.*` | 整个文件，由 `model_file_resolver.*` 取代 | 2 |
| `DeploymentPrepareOptions::model_root_dir`（I/O 设计 6.4 保留的字段） | 改为 `pipeline_dir`：Pipeline JSON 所在目录，为空时只检查写法 | 2 |
| `alg_pipeline_tool validate-io` 的 `--model-root` | 删除 | 2 |
| `tools/verify_selection.py` 的 `--model-root`、`--pipeline-root` 和 `build_run_conf` 中的路径重定位；`tools/dev_recipe.py` 的 `--model-root` 和 `deployment_root()`；Studio 的"模型目录"（`model_root`） | 删除：资源一律相对方案所在目录 | 2 |
| `models/` 目录 | 并入 `configs/`（第 12 节） | 2 |
| `doc/developer_guide.md` 中关于 `normalize` 的迁移说明 | 删除 | 2 |

## 12. 目录合并：`models/` 并入 `configs/`

方案和它用到的文件放在同一目录，`configs/` 就是一个完整的部署目录，与外部平台配置的目录结构一致。

| 项目 | 改动 |
| --- | --- |
| `models/README.md` | 内容并入 `configs/README.md` 的"权重与资源"一节 |
| `models/asset_manifest.json`、`models/kite_text_run.json`、`models/kite_vision_run.json` | 移到 `configs/` |
| `models/.gitkeep` | 删除；`models/` 目录不再存在 |
| `.gitignore` | 删除 `models/` 的保留规则；词表的忽略规则改为 `configs/vocab.txt`、`configs/*_vocab.txt`。权重已按扩展名全局忽略 |
| `scripts/fetch_real_test_models.sh`、`scripts/run_real_model_e2e.sh` | 下载和读取目录改为 `configs/` |
| `.github/workflows/ci.yml` | 缓存路径、缓存键（`hashFiles('configs/asset_manifest.json')`）和 kite 测试的环境变量 |
| `demo/profiles.json`、`demo/profiles_kite.json` | 不变（`.conf` 仍在 `configs/`） |
| `tests/fixtures/pipelines/cross_rerank/` | 方案本来就与生成的模型在同一目录，只改字段 |
| 本地已下载的权重 | 一次性移到 `configs/`。命令写在 PR 描述里，不提交脚本 |

## 13. 对外接口与工具

### 13.1 Catalog JSON

| 键 | 变化 |
| --- | --- |
| `models[]` | 每项是一个实现：`model_type`（实现名）改为 `impl_name`；`capability` 改为 `model_type`；新增 `backends`，列出能运行它的后端（由协议推出）；`config_fields` 来自 `params.Fields()`，文件参数带 `"file": true`，可选参数非必填、无默认值 |
| `backends[]` | `backend_type` 不变；`config_fields` 同上 |
| `nodes[].model_dependencies[]` | `capability` 改为 `model_type` |

### 13.2 `alg_pipeline_tool`

| 命令 | 变化 |
| --- | --- |
| `describe-model` | 改为 `describe-model TYPE BACKEND`：显示该组合选出的实现名及其参数；`describe-backend` 参数为后端名。输出键同 13.1 |
| `validate` / `plan` | 给出 `FILE` 时，按其所在目录解析文件；`--stdin` 只检查写法 |
| `validate-io` | 删除 `--model-root`；文件按 Pipeline JSON 所在目录解析 |
| `resolve-conf` | `effective_pipeline.models[]` 写入解析后的 `file` 和两组归一化参数（文件参数为绝对路径）；`model_paths` 改为 `model_files`，每个文件一项 `{"model", "path", "resolved"}`，其中 `model` 为条目的 `name`，`path` 为该值的 JSON 指针 |
| `export-schema` | 模型条目的 schema：`type`、`name`、`file`、`backend` 必填，`params` 可选，`additionalProperties: false`；`type` 为类别枚举（取自已注册实现）；`backend.type` 为后端枚举；`params` 与 `backend.params` 按（类别, 后端）组合用 if/then 分支，schema 由选出实现和后端的 `config_fields` 生成 |
| 修复建议（`src/cli/pipeline_remediation.cpp`） | `unknown_model_type` 的候选为类别；`unknown_backend` 与新增的 `backend_protocol_mismatch` 的候选为该类别可用的后端；`unknown_model_reference`、`model_type_mismatch` 的候选为类别相同的模型 `name` |
| `alg_show` | 每个模型显示 `name`、类别、选出的实现名、后端名和 `file` |

### 13.3 Studio

- `tools/pipeline_studio/web/app.js`、`workbench.js`、`editor.js`：
  - 模型表单：类别（取自 Catalog `models[]` 中出现的 `model_type`）、模型名、后端（按类别过滤，取自 `models[].backends`）、模型文件。选出的实现名只作提示显示。两组参数复用现有的参数编辑组件；文件参数提示"相对方案所在目录的文件名"；
  - 节点的模型下拉框按依赖的 `model_type` 过滤；
  - 应用资源选型时，按清单的新模板写入条目（13.4）。
- `tools/pipeline_studio/server.py`：读取 `resolve-conf` 的 `model_files`；删除"模型目录"（`model_root`）及其路径重定位，方案与资源在同一目录。
- `tools/pipeline_studio/README.md`。
- 测试：`tests/tooling/test_pipeline_studio.py` 及 `studio_*.mjs`。

### 13.4 资源清单与 Python 工具

资源清单（`configs/asset_manifest.json`、`tests/fixtures/asset_manifest_test.json`）的选型模板改为新的条目形状，`paths` 中的指针随之改变。清单中的文件名与方案里的写法相同，都相对使用它的方案所在目录（测试清单里的 `demo/fixtures/mock/artifacts/neutral-llm.fixture` 改为 `artifacts/neutral-llm.fixture`）：

```json
{
  "id": "bge_embedding-onnxruntime",
  "label": "bge_embedding / onnxruntime",
  "model": {
    "type": "embedding",
    "name": "embed_model",
    "file": "bge_base_zh_v1.5.onnx",
    "params": { "tokenizer_file": "bge_base_zh_v1.5_vocab.txt" },
    "backend": { "type": "onnxruntime" }
  },
  "paths": {
    "/file": "bge_base_zh_v1.5.onnx",
    "/params/tokenizer_file": "bge_base_zh_v1.5_vocab.txt"
  },
  "files": ["bge_base_zh_v1.5.onnx", "bge_base_zh_v1.5_vocab.txt"]
}
```

- `tools/verify_selection.py`：
  - 资源一律相对 `--pipeline` 所在目录；删除 `--model-root`、`--pipeline-root`；清单默认为 `configs/asset_manifest.json`；
  - 按模板中的类别与后端名匹配选型；`/file` 是必有的路径；
  - `evaluate` 把临时方案和 `.conf` 以隐藏文件写在方案所在目录，运行后删除；`build_run_conf` 不再改写路径。
- `tools/dev_recipe.py`：删除 `--model-root` 与 `deployment_root()`；`--pipeline` 必须放在资源所在目录；`resolve-conf` 的 `--root` 使用方案所在目录。
- `tests/tooling/generate_scaffold_fixtures.py`：生成的测试改用新的解析器签名。

## 14. 实施阶段

| 阶段 | 内容 | 依赖 |
| --- | --- | --- |
| 1 | 模型、后端改用统一参数机制；配置格式不变 | I/O 设计阶段 0 |
| 2 | 新的模型配置格式 | 本设计阶段 1；I/O 设计阶段 2 |

每个阶段一个 PR，各自通过 `./scripts/run_all_tests.sh`。配置格式只在阶段 2 切换一次；每个 PR 都完整迁移，不留兼容。

### 阶段 1：统一参数机制，配置格式不变

**目标**：引擎内部只剩一种参数写法，只校验一次。JSON 不变，可以单独评审。

| 文件 | 改动 |
| --- | --- |
| `include/contracts/parameter_set.h`（新增） | 5.2 中的 `Fields()`、`Parse`、`Get<P>()`。`Integer()`、`Effective()` 由 I/O 设计阶段 2 加入 |
| `include/contracts/parameters.h` | `std::optional` 参数（5.3） |
| `include/contracts/config_schema.h` | 删除 `ConfigValueOrDefault` |
| `include/engine/inference_definition.h`、`model_registry.h`、`backend_interface.h`、`model_runtime_factory.h` | 5.5 中的 `params` 与 `Params<P>()`。本阶段保留 `ModelCreateContext::model_resource_root`、`BackendLoadSpec::model_path` 和现有的类别/实现名字 |
| `src/engine/runtime/model_runtime_factory.cpp` | 5.5 的步骤 2 |
| `src/engine/models/*`、`src/engine/backends/*` | 四段写法（5.1）；删除第 11 节中阶段 1 的复查；bge 的 `embedding_dim`、`max_length` 改为可选参数并使用 `ResolveFromModel`（本阶段仍叫 `max_length`）；whisper、kite 的参数说明改为中文 |
| `src/engine/models/common/from_model.h`（新增）、`src/engine/models/bge_common/bert_model_support.*` | `ResolveFromModel`、`StaticOutputDim`、`StaticSequenceLength` |
| `dev_support/inference/*` | 测试模型、测试后端改用四段写法，参数名不变 |
| `src/core/pipeline_validator.cpp`、`src/core/pipeline_catalog.cpp`、`src/cli/pipeline_json_schema.cpp` | 字段来自 `params.Fields()`；语义校验改为调用 `params.Parse` |
| `tests/contract/architecture/test_layer_header_views.cmake` | 允许模型执行层包含 `contracts/parameters.h`、`contracts/parameter_set.h` |
| 测试 | `tests/unit/engine/*`、`tests/contract/catalog/*`；第 15 节阶段 1 的用例 |
| 文档 | `.agents/skills/edgeflow-model-developer`、`edgeflow-backend-developer`；`llm-edgeflow-developer-guide/references/model-execution.md`；`doc/developer_guide.md` 中模型与后端的编写部分 |

**验收**：
- 方案文件不变；Operator 黄金测试不变并通过；
- `describe-model`、`describe-backend` 的参数名、类型、默认值和范围与改造前一致。例外：`bge_embedding` 的 `embedding_dim`，以及 `bge_embedding`、`bge_reranker` 的 `max_length` 改为可选；whisper、kite 的说明改为中文；
- `git grep` 不再出现 `ConfigValueOrDefault`、`ParseLlamaCppConfig`，以及模型和后端的 `validate_config`；
- 门禁通过。

### 阶段 2：新的模型配置格式

建议按下列顺序提交。中间提交尽量保持可编译；PR 最后必须通过门禁。

| 提交 | 内容 | 主要文件 |
| --- | --- | --- |
| C1 类别、名字与实现改名 | 6.1；配置格式暂时不变 | `include/engine/*`、`src/engine/**`、`include/core/*`、`src/core/*`、`include/nodes/function_node.h`、`model_binding.h`、`model_calls.h`、`dev_support/inference/*`、`src/cli/*`、相关测试 |
| C2 节点参数 | 第 8 节：`GenerateOptions`、`TranscribeOptions`、`IAsrModel`、`AsrCall`、AsrTranscribeNode、`ValidateModels`；向量归一化移到模型、删除 `EmbeddingOptions`；删除 PromptGuidedLlmNode 的 `prompt_prefix`；Qwen、Whisper、两个向量模型的相应改动 | `include/contracts/inference_payloads.h`、`include/engine/model_interface.h`、`include/nodes/*`、`src/common_nodes/asr_transcribe_node.cpp`、`src/common_nodes/text_embedding_node.cpp`、`src/custom_nodes/prompt_guided_llm_node.cpp`、`src/engine/models/qwen_causal_lm/*`、`src/engine/models/whisper_asr/*`、`src/engine/models/bge_embedding/*`、`src/engine/models/generated_text_embedding/*`、`dev_support/inference/*`、相关测试 |
| C3 Core 新格式 | 4.1、4.3、第 9 节：条目解析、按类别和后端选出实现、校验顺序与诊断、`UNUSED_MODEL`；`ModelRegistry::FindImplementation` 与注册审计；测试夹具改用 `kFixture` 协议和 `fixture_backends`，测试源码中自行注册的模型同样改写（4.1）；删除路径检查 | `include/core/pipeline_config.h`、`src/core/pipeline_config.cpp`、`pipeline_config_structure.cpp`、`pipeline_validator.*`、`pipeline.cpp`、`include/core/session_context.h`、`diagnostic_code.h`、`remediation_cause.h`、`include/engine/inference_definition.h`、`model_registry.h`、`src/engine/runtime/*`、`src/adapter/shared_algorithm_runtime.cpp`、`dev_support/inference/*`、`tests/unit/core/*`、`tests/integration/pipeline/*` |
| C4 文件参数与批大小 | 4.2、5.4；`ResolveModelFiles`、`pipeline_dir`；删除 `model_resource_root` 和 bge、kite 的路径代码；bge、reranker 的 `tokenizer_file` 改为必填（原默认 `vocab.txt`，方案和测试都已显式写出）；kite 的 `run_config_file` 改为 `std::optional`；删除模型上的批大小，测试后端加 `fixed_batch_size`；参数改名（6.2） | `include/contracts/parameters.h`、`config_schema.h`、`config_schema_validation.h`、`path_utils.h`；`src/adapter/model_file_resolver.*`、`deployment_preparation.*`、`io_plan_resolver.*`、`shared_algorithm_runtime.cpp`、`operator/operator_config_resolver.cpp`；`src/engine/**`；`dev_support/inference/*`；`tests/unit/adapter/*`、`tests/unit/engine/*` |
| C5 命令行工具 | 13.1、13.2 | `src/core/pipeline_catalog.cpp`、`src/cli/*`、`tests/RuntimeTests.cmake` |
| C6 Studio 与 Python 工具 | 13.3、13.4 | `tools/pipeline_studio/*`、`tools/verify_selection.py`、`tools/dev_recipe.py`、`tests/tooling/*` |
| C7 目录合并与迁移 | 第 12 节；按 4.5 迁移全部方案、夹具、清单和内嵌配置 | 4.5 与第 12 节列出的文件；`tests/contract/*`、`tests/integration/*`、`tests/e2e/*` |
| C8 文档与规则 | 第 16 节；删除本文件 | 见第 16 节 |

**验收**：
- 8 个业务的 Operator 黄金测试只改配置路径和内容，期望值一律不改，全部通过；
- 改造前后分别运行 `alg_demo --suite smoke`，`results.jsonl` 的 `output` 字段逐条一致；
- 全部方案改为新格式并通过 `validate` 和 `plan`；kite 方案在 kite 构建中验证；
- 第 15 节的新增测试全部通过；
- 除 `doc/CHANGELOG.md` 的历史条目外，`git grep` 不再出现：Pipeline JSON 的 `model_id`、`model_config`、`backend_config`、`pooling_strategy` 键和模型条目里的 `model_path`、`model_type` 键（Operator 的 Create 参数 `model_path` 不变）；代码中的 `Capability()`、`ModelCapabilityTraits`、`kModelType`、`model_id`、`EmbeddingOptions`、`model_resource_root`、`INVALID_MODEL_PATH`、`DUPLICATE_MODEL_ID`、`UNKNOWN_MODEL_CONFIG_FIELD`、`UNKNOWN_BACKEND_CONFIG_FIELD`；`models/` 目录（本文件随阶段 2 删除）。不为这些旧名字编写"会被拒绝"的专门测试，它们按通用的未知字段规则处理；
- 第 11 节所列内容全部清理，没有新增任何兼容分支、别名或迁移代码；
- 门禁通过；本地有真实权重时运行 `scripts/run_real_model_e2e.sh`。

## 15. 测试计划

| 范围 | 用例 | 阶段 |
| --- | --- | --- |
| 参数机制 | 可选参数：不写时为空，写了按范围校验；可选参数声明 `Required()` 或 `Default()` 时报错；Catalog 中可选参数非必填、无默认值；`ParameterSet::Parse` 对原始配置和已归一化配置结果一致；`Get<P>` 类型不符时抛出 `std::logic_error` | 1 |
| 模型与后端参数 | 每个实现：`{}` 使用默认值、覆盖生效、`Validate` 规则（llama_cpp 的 `decode_batch_size`、vision 的 `prompt`、bge 的 `output_name`）；原来针对 `Create` 复查的用例改为在参数层断言 | 1 |
| 从模型读取 | `ResolveFromModel` 的四种情况；bge 从固定形状读取 `embedding_dim` 和编码长度；配置值与固定形状不一致时报错；动态形状且未配置 `embedding_dim` 时报错并提示填写 | 1 |
| 工厂 | 两组参数各解析一次；参数非法时不创建后端提供者 | 1 |
| 文件路径 | `ResolveFileUnderDirectory`：空值、绝对路径、盘符、`..`、符号链接越界、子目录、文件不存在（允许）；`ResolveModelFiles`：`file`、`tokenizer_file`、`run_config_file` 解析为绝对路径，非文件参数不变，选不出实现的条目跳过，没有目录时只检查写法；kite 的 `vision.mmproj` 相对 run config 所在目录 | 2 |
| Core | 条目缺键、未知键、`backend` 缺 `type`；`UNKNOWN_MODEL_TYPE`；`UNKNOWN_BACKEND` 与 `BACKEND_PROTOCOL_MISMATCH` 只建议该类别可用的后端；按类别和后端选出的实现与第 4.1 节的表一致；`DUPLICATE_MODEL_NAME`；节点处的 `MODEL_TYPE_MISMATCH`；参数诊断码与路径；`UNUSED_MODEL`，以及存在未知节点类型时不报；同一类别的两个模型分别被两个节点绑定 | 2 |
| 注册审计 | 测试中再登记一个与生产实现同类别、同协议的模型时，`Init` 失败并列出冲突的实现；测试夹具使用 `kFixture` 后，mock Demo 与测试程序中每个（类别, 后端）只对应一个实现；两个同类别的 `kFixture` 测试模型列出不同的测试后端时不冲突；一个测试模型列出多个后端时都能选中；不经过 `GlobalInit` 时 Core 对选出多个实现报 `REGISTRY_CONFLICT` | 2 |
| 节点 | `system_prompt` 进入 ChatML 的 system 段，`random_seed` 派生种子；同一个 LLM 被两个节点以不同的 `system_prompt` 使用；ASR 按节点的 `language` 转写；所绑模型不支持该语言时 Create 报 `NODE_INIT_FAILED`；`ValidateModels` 单测；向量模型 `normalize` 为 false 时输出不归一化，两个节点结果一致 | 2 |
| 批大小 | bge 只按会话的批策略切批；onnxruntime 的 `max_batch_size` 生效；测试后端的 `fixed_batch_size` 产生固定批 | 2 |
| 工具 | catalog 的新键名与 `backends`、`describe-model TYPE BACKEND`、describe-backend、resolve-conf 的 `model_files`、validate-io 不再接受 `--model-root`、export-schema 能校验全部方案、修复建议、Studio 的 Python 与浏览器测试、verify_selection、dev_recipe、资源清单校验 | 2 |
| 集成 | Operator 黄金测试、全业务集成测试、Demo smoke 结果不变；有真实权重时运行 e2e | 2 |

## 16. 文档与规则更新

阶段 1 只更新与编写方式有关的部分（见阶段 1 的文件表），其余在阶段 2 的 C8 中完成。

| 文件 | 改动 |
| --- | --- |
| `configs/README.md` | 新的模型条目、文件规则、参数归属规则；并入原 `models/README.md` 的下载说明 |
| `doc/developer_guide.md` | 模型与后端的编写方式（四段写法、`Validate`、`ResolveFromModel`、`.File()`）；条目示例；删除 `validate_config`、`ConfigValueOrDefault` 和 `normalize` 迁移说明；改写"模型能力不在 JSON 中重复声明"：条目写类别，实现由类别和后端选出 |
| `doc/architecture.md`、`doc/architecture_flow.puml` | 模型文件解析的位置与规则；运行 `./scripts/render_architecture_diagrams.sh --generate` 重新生成 SVG |
| `doc/README.md`、`doc/kitellm.md`、`doc/VERIFIABLE_SELECTION.md`、`doc/solutions/translate.md`、`doc/dev_guide/business_onboarding.md`、`doc/dev_guide/custom_node_concepts.md` | 配置示例、目录、`system_prompt` 的位置、资源清单与工具参数 |
| `src/custom_nodes/README.md` | `system_prompt` 的位置；`ValidateModels` |
| `tools/pipeline_studio/README.md` | 模型表单、资源目录 |
| `.agents/skills/edgeflow-model-developer`、`edgeflow-backend-developer` | 四段写法；`kImplName`（`kBackendType` 不变）；实现由类别和后端选出、每个（类别, 后端）只能有一个实现；参数归属规则；`Create`/`Load` 只做依赖已加载内容的检查 |
| `.agents/skills/edgeflow-node-developer`、`llm-edgeflow-developer-guide/references/capability-nodes.md` | 按类别核对模型绑定；`ValidateModels`；生成参数新增两项；偏业务的后处理写在节点里（第 3 节）；向量归一化在模型上 |
| `.agents/skills/llm-edgeflow-developer-guide/references/model-execution.md`、`orchestration.md`、`integration.md` | 参数机制、诊断、文件解析的位置 |
| `.agents/skills/pipeline-composer`（含 `references/workflow.md`）、`edgeflow-solution-planner`、`json-prompt-solution` | 条目写法、文件规则、`system_prompt` 的位置 |
| `plans/FRAMEWORK_SIMPLIFICATION_PLAN.md`、`plans/DEVELOPER_EXPERIENCE_RFC.md` | 按第 10 节改写对应的行；O-1 记为已处理 |
| `doc/CHANGELOG.md` | 各阶段的用户可见变化 |

## 17. 风险与不做的事

| 项目 | 处理 |
| --- | --- |
| 改动面大：阶段 2 涉及 150 个以上的文件 | 分两个阶段、按 C1–C8 提交；黄金测试期望值不改；配置只在阶段 2 切换一次 |
| 本地已下载的权重换了位置 | PR 描述给出一次性移动命令；下载脚本改为写入 `configs/` |
| 删除默认值后，代码默认值的变化会影响方案 | 默认值属于组件契约，修改时按契约变更评审（CONTRIBUTING §3），评估所有没有覆盖它的方案 |
| bge ONNX 的输出维度是否为固定值 | 实施时用真实权重确认；不是固定值则保留 `embedding_dim` |
| 不做：`models` 改为以 `name` 为键的对象 | JSON 解析器遇到重复键会静默覆盖，反而失去现有的重复检查；也与 `io`、`pipeline` 的数组写法不一致 |
| 实现名不写进配置 | 靠注册审计保证每个（类别, 后端）只有一个实现。同一（类别, 后端）以后需要完全不同的处理流程时，只能并入已有实现，这是唯一需要修改已有实现的扩展场景（4.1）；目前没有这种需求 |
| 不做：按类别自动绑定模型 | 同一类别可以有多个模型 |
| 不做：多个条目引用同一份权重时共享加载 | 各条目各自加载。触发条件：出现由此造成的内存瓶颈 |
| 不做：思考模式、LoRA | 归属已定（第 3 节）：思考模式是 LLM 生成参数的可选项，用 `ValidateModels` 在 Create 时核对；LoRA 的适配器列表是模型参数，选用哪一个是节点参数 |
| 不做：kite 维度的加载时探测 | 固定版本没有查询接口；为一个维数在加载时跑一次生成不值得 |
| 不做：后端参数跨后端统一命名 | 沿用各推理库的术语（6.2） |
| `ConfigFieldDefinition` 的 8 元组构造函数 | 节点的共享生成参数和文本处理类节点仍在使用；由[节点设计](PIPELINE_NODE_DESIGN.md)阶段 1 删除 |

## 18. 已确认的决定

| 决定 | 内容 |
| --- | --- |
| 条目形状 | `type`、`name`、`file`、`params`（可省略）、`backend: {"type": 后端名, "params": 参数}`；`models` 是数组 |
| 类别 | `type` 表示类别：llm、embedding、rerank、ocr、asr；可用类别取自注册表，不另外维护列表。图像类先用 `ocr`，需要通用图像问答时再新增 `vlm` |
| 身份与绑定 | `name` 唯一；同一类别可以有多个模型；节点的 `bind_model` 必填，引用 `name`，不按类别自动绑定 |
| 实现 | 不写进配置，由类别和后端选出；注册审计保证每个（类别, 后端）只有一个实现；差异用模型参数表达 |
| 测试模型 | 用户确认（4.1）：测试模型只要求专用协议 `kFixture`，并在 `ModelDefinition::fixture_backends` 中列出它能运行的测试后端；`kFixture` 模型只与列出的后端匹配。共享夹具和测试源码中自行注册的模型都这样写。Core 按类别和后端选出多个实现时报 `REGISTRY_CONFLICT` |
| 命名 | 每个条目 `type` 说"是哪一类"、`name` 说"是哪一个"，与 `io`、节点一致；代码字段带实体前缀（`model_type`、`model_name`、`backend_type`），实现名为 `impl_name` |
| 参数归属 | 同一个已加载的模型、不同节点能取不同值的放节点；其余放模型或后端。偏业务的后处理写在节点里：调用模型后再处理 |
| 移到节点 | `system_prompt`、`random_seed`、whisper 的 `language` |
| 移到模型 | 向量的 `normalize`（原在 TextEmbeddingNode） |
| 留在模型 | `pooling`、`prompt_prefix`、`prompt_suffix`、`add_bos`，以及 `vision_document` 的 `prompt` 和 `max_tokens` |
| 批大小 | 只在后端 |
| 从模型读取 | 只在有真实接口时；kite 固定版本没有查询接口，`generated_text_embedding.embedding_dim` 必填 |
| 文件路径 | 相对 Pipeline JSON 所在目录（外部约定）；方案与资源都放在 `configs/` |
| 参数机制 | 与节点、converter 相同；`ParameterSet` 三处共用 |
| 没有被使用的模型 | 报错 |
| 节点要求、所绑模型做不到时 | Create 报错（`ValidateModels`） |
| 默认值 | 配置中等于默认值的参数一律删除，只写与默认值不同的项（附录 A；RFC 待决事项 O-1） |
| 不做兼容 | 不保留旧字段别名、不识别旧格式、不提交迁移代码；不为旧名字编写专门的拒绝测试 |

## 附录 A：等于默认值、迁移时删除的参数

统计范围为 `configs/` 下的方案，方案名省略前缀 `pipeline_` 和后缀 `.json`。迁移时全部删除。

| 实现 | 参数 | 值 | 方案 |
| --- | --- | --- | --- |
| `bge_embedding` | `do_lower_case` | `true` | doc_qa_cpu、doc_qa_rerank_cpu、doc_qa_rerank_kite |
| `bge_embedding` | `max_length` | `512` | 同上 |
| `bge_embedding` | `output_name` | `"last_hidden_state"` | 同上 |
| `bge_embedding` | `pooling_strategy` | `"cls"` | 同上 |
| `bge_reranker` | `do_lower_case` | `true` | cross_rerank_cpu、doc_qa_rerank_cpu、doc_qa_rerank_kite |
| `bge_reranker` | `max_length` | `512` | 同上 |
| `bge_reranker` | `output_name` | `"logits"` | 同上 |
| `bge_reranker` | `score_activation` | `"sigmoid"` | 同上 |
| `generated_text_embedding` | `add_bos`、`max_tokens`、`pooling` | `false`、`1`、`"last"` | doc_qa_kite_generated_embeddings |
| `llama_cpp` | `check_tensors`、`decode_batch_size`、`n_gpu_layers` | `false`、`512`、`0` | dialogue_audit_default、doc_qa_cpu、doc_qa_default、doc_qa_rerank_cpu、doc_qa_rerank_default、entity_extract_cpu、entity_extract_default、translate_cpu |
| `llama_cpp` | `n_threads`、`n_threads_batch` | `0`、`0` | 上一行除 translate_cpu 外的 7 份 |
| `llama_cpp` | `context_size` | `2048` | translate_cpu |
| `onnxruntime` | `max_batch_size` | `4` | cross_rerank_cpu、dialogue_audit_default、dialogue_audit_kite、doc_qa_cpu、doc_qa_default、doc_qa_kite、doc_qa_rerank_cpu、doc_qa_rerank_default、doc_qa_rerank_kite（共 14 处） |
| `onnxruntime` | `intra_op_num_threads`、`inter_op_num_threads`、`graph_optimization_level` | `2`、`1`、`"all"` | cross_rerank_cpu、doc_qa_cpu、doc_qa_rerank_cpu、doc_qa_rerank_kite |
| `qwen_causal_lm` | `add_bos` | `false` | 与 `llama_cpp` 第一行相同的 8 份 |
| `vision_document` | `patch_size` | `16` | ocr_invoice_qa_kite |
| `whisper_asr` | `max_audio_seconds`、`max_output_bytes` | `30`、`65536` | audio_asr_intent_cpu |
| `whisper_cpp` | `n_threads` | `4` | audio_asr_intent_cpu |

下列参数因被删除或移到节点而不再写在模型上，它们的现有值都等于默认值，不需要搬到节点：
- `bge_reranker.max_batch_size`（6 份）；
- `qwen_causal_lm.random_seed: -1`（13 份）和 `system_prompt: ""`（7 份）；
- `whisper_asr.language: "zh"`（1 份）。

与默认值不同、迁移后保留的值有：
- `llama_cpp.context_size`（1024、512）；
- `translate_cpu` 的 `n_threads`、`n_threads_batch`（4）；
- `vision_document.max_tokens`（256）；
- 各 `tokenizer_file`、`run_config_file`；
- `generated_text_embedding` 的 `embedding_dim` 与提示词；
- `bge_embedding.embedding_dim`（见 4.5）。

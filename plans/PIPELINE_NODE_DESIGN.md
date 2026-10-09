# Pipeline 节点与连线改造设计

> **状态**：参数声明、文本处理类、节点与连线格式切换（步骤 4–5、8）已实施；工具功能在步骤 9 完成。
> **基线**：`main@cda1f5c`。
> **实施**：按[实施总说明](PIPELINE_REFACTOR_GUIDE.md)的步骤进行；本文中关于 PR 划分和门禁的说法与总说明不一致时，以总说明为准。
> **性质**：跨多个 PR 的工作计划，规则见 [plans/README](README.md) 与 [CONTRIBUTING](../CONTRIBUTING.md) §3。
> 各阶段 PR 的描述引用本文对应章节；最后一个阶段合入时删除本文，形成的规则写进现行指南。
> **目标**：简化系统、降低开发心智、提升易用性。每项改动都应能落到下面某一条：概念更少、写法只有一种、名字一看就懂、出错更早。
> **不做兼容**：项目尚未发布，没有兼容使用方。只实现新方案：不保留旧字段别名、不识别旧格式、不提交迁移代码、不写针对旧名字的测试。
> **依赖**：
> - [Pipeline I/O 改造设计](PIPELINE_IO_DESIGN.md)：阶段 1 依赖它的阶段 0（参数声明下沉到 `contracts/`）；阶段 2 依赖它的阶段 2。
> - [Pipeline 模型配置改造设计](PIPELINE_MODEL_DESIGN.md)：阶段 2 依赖它的阶段 2。三份设计的格式切换按 I/O、模型、节点的顺序进行。
>
> **分层**：四层依赖方向不变。连线由 Core 的 `PipelineValidator` 解析；converter 端口由接入层声明；节点只通过逻辑端口和模型能力工作。
> **不在范围**：
> - `.conf`、`pipe_path`：外部契约，不改；
> - `io`、`models` 条目本身的写法：见上面两份设计。本文只给输出项增加 `inputs`、把 converter 端口改为逻辑名（第 4 节）；
> - 新功能：思考模式、每个 endpoint 单独设置的生成参数。本文只确定它们出现时放在哪里（6.1）。

## 1. 目标

改造完成后：

1. 节点条目与 `io`、`models` 同一种写法：`type` 说"是哪一类"（节点类型），`name` 说"是哪一个"（节点名），参数放在 `params`。去掉 `id`、`node_type`、`config`、`outputs`。
2. 连线直接引用"节点名.端口名"，不再为每条连线起中间数据名；`io` 的输出项用同样的 `inputs` 写明回包的数据来源。DAG 仍由校验器自动推导。
3. 节点类型名与其他名字一样用 snake_case，例如 `llm_generate`。
4. 节点参数只有一种声明写法：`Field`，包括映射、对象数组和共享参数组；删除 8 元组和手写解析。控制命令的参数格式由参数声明生成，不再手写 schema。
5. LLM 生成节点通用化：一个节点配一个或多个 endpoint，一个就是单次生成，多个就是对同一输入问多个问题。
6. 能从连线推出的事实不用配置：向量缓存与共享候选由输入数据的生命周期决定。

改造后的方案（文档问答，cpu）：

```json
{
  "io": {
    "input":  [ { "type": "doc_in", "name": "doc_qa" } ],
    "output": [ { "type": "doc_out", "name": "doc_qa",
                  "inputs": { "answer_text": "generate_answer.text",
                              "intent":      "match_intent.matches",
                              "chunk_count": "chunk_docs.chunk_counts" } } ]
  },
  "models": [
    { "type": "embedding", "name": "embed_model", "file": "bge_base_zh_v1.5.onnx",
      "params":  { "tokenizer_file": "bge_base_zh_v1.5_vocab.txt" },
      "backend": { "type": "onnxruntime" } },
    { "type": "llm", "name": "llm_model", "file": "qwen2.5-0.5b-instruct-q4_k_m.gguf",
      "backend": { "type": "llama_cpp", "params": { "context_size": 1024 } } }
  ],
  "pipeline": [
    { "type": "text_chunk", "name": "chunk_docs", "params": { "chunk_size": 60 },
      "inputs": { "text": "input.doc_text" } },
    { "type": "text_embedding", "name": "embed_query", "params": { "bind_model": "embed_model" },
      "inputs": { "text": "input.query_text" } },
    { "type": "text_embedding", "name": "embed_chunks", "params": { "bind_model": "embed_model" },
      "inputs": { "text": "chunk_docs.chunks" } },
    { "type": "vector_top_k", "name": "retrieve",
      "inputs": { "queries": "embed_query.embedding", "candidates": "embed_chunks.embedding",
                  "candidate_texts": "chunk_docs.chunks" } },
    { "type": "text_template", "name": "build_prompt",
      "params": { "template": "【参考片段】: {{context}}\n【用户提问】: {{primary}}\n请基于参考片段给出精确解答:" },
      "inputs": { "primary": "input.query_text", "context": "retrieve.ranked" } },
    { "type": "text_rule_match", "name": "match_intent",
      "params": { "default_category": "GENERAL_QA", "default_score": 0.9,
                  "categories": { "AFTER_SALES_REFUND": ["退款", "退货", "换货"],
                                  "TECH_ARCHITECTURE": ["架构", "设计", "C++", "算法框架", "ONNX", "llama.cpp"] } },
      "inputs": { "text": "input.query_text" } },
    { "type": "llm_generate", "name": "generate_answer",
      "params": { "bind_model": "llm_model", "temperature": 0.3, "endpoints": { "answer": {} } },
      "inputs": { "input": "build_prompt.text" } }
  ]
}
```

整份方案只有一条规则：每个条目是 `type`、`name` 加可省略的 `params`，再加各自特有的键；列表都是数组；所有名字都用 snake_case（模型名可以自由取，例如 `bge-base-sft`）。

对开发者的变化：

| | 现在 | 改造后 |
| --- | --- | --- |
| 节点条目 | `id`、`node_type`、`config`、`inputs`、`outputs`、`depends_on` | `type`、`name`、`params`、`inputs`、`depends_on` |
| 连线 | 输出写到数据名、输入按数据名读取；每条连线起一个名字，两端各写一遍 | 输入直接写 `节点名.端口名`；不写 `outputs` |
| 回包的数据来源 | 输出 converter 要求方案里存在特定数据名（如 `llm_answers`），JSON 中看不出 | 输出项的 `inputs` 写明 |
| 节点类型名 | 大驼峰加 `Node` 后缀，如 `LlmGenerateNode` | snake_case，如 `llm_generate` |
| 节点 ID | `node_6_LlmGenerateNode`，插入节点后序号就过时 | 按用途取的 `name`，如 `generate_answer` |
| 参数声明 | `Field`；另有 8 元组加手写解析 | 只有 `Field` |
| 控制命令 | 字段命令之外，复杂命令手写 JSON schema 和更新函数 | 只有字段命令，格式由参数声明生成 |
| 一个节点问多个问题 | 要拆成多个节点 | `llm_generate` 配多个 endpoint |
| 向量缓存、共享候选 | `lifetime`、`candidate_scope` 两个参数，必须与输入一致 | 由输入数据的生命周期决定 |

## 2. 现状与问题

| 编号 | 问题 | 依据 |
| --- | --- | --- |
| N1 | 节点条目与 `io`、模型的写法不一致：`node_type` 指实现，模型的 `type` 指类别；身份叫 `id`，参数叫 `config` | 模型设计 6.1 记录的不一致 |
| N2 | 每条连线都要起一个中间数据名并在两端各写一遍 | 文档问答方案有 10 个这样的名字（`doc_chunks`、`prompt_text`、`llm_answers` 等） |
| N3 | io 的数据名是隐形约定 | `doc_out` 要求方案中存在 `llm_answers`、`intent_matches`、`doc_chunk_counts`，定义在 `include/adapter/biz_blackboard_keys.h` |
| N4 | 没写进 `outputs` 的端口仍按端口名发布，两个节点都不写时报 `DUPLICATE_PORT_PRODUCER` | 仓库方案中 141 个输出全部显式写了映射，没有方案依赖这个默认行为 |
| N5 | 节点类型名是 JSON 中唯一不用 snake_case 的名字，`Node` 后缀与上下文重复 | converter、模型、后端、参数、端口都是 snake_case |
| N6 | 节点 ID 用序号命名，插入节点后就过时；而控制命令的定向信封和命令行 `edit` 都用它定位节点 | `node_<序号>_<类型>` 由 `src/cli/pipeline_authoring.cpp` 生成；`doc/dev_guide/first_control.md` |
| N7 | 参数声明有两种写法：4 个节点用 `Field`；4 个文本处理节点和两个 LLM 节点的生成参数用 8 元组加手写解析 | `Field` 不支持映射、对象数组和嵌套成员 |
| N8 | 生成参数：8 元组加按字符串读取；`GenerateParameters` 两个重载加 `NonDeduced` 技巧；默认值在结构体和声明里各写一遍；`max_tokens` 默认值随节点不同（128 / 512） | `include/nodes/generate_options_config.h` |
| N9 | 一个 LLM 节点只能问一个问题，多个问题要拆成多组模板节点加 LLM 节点 | 实体抽取需要一次抽多个字段 |
| N10 | `lifetime`、`candidate_scope` 只是重复输入数据的性质 | 3 个方案写了它们，输入都来自 `text_corpus_source` |
| N11 | `text_rerank` 有三组输入、只能接一组；方案只用到其中一组 | `pairs`、`candidate_texts` 只出现在单元测试中 |
| N12 | `vector_top_k` 的 `candidate_texts` 可选，不接时输出没有文本，下游用不了 | 12 个方案全都接了 |
| N13 | 每个节点都手写一遍 `bind_model` 的说明 | "引用 models[].model_id；所选模型必须提供 llm 文本生成能力。" |
| N14 | 方案中写了不少默认值 | `configs/` 中 110 个节点参数值（不含 `bind_model`）有 25 个等于默认值（附录 C） |
| N15 | 复杂控制命令的参数格式另外手写一份 JSON schema，与参数声明重复 | `text_template` 的 `TemplateControlSchema`、`text_rule_match` 的 `RuleControlSchema` |
| N16 | 文本处理节点有接不上的输入、同一内容的两种输入，以及写在字符串里的 JSON | `text_template` 的 `attributes`（`TextAttributesBatch` 没有任何生产者）、`context` 与 `context_text`；`text_corpus_source` 的 `trigger` 只为排顺序；`structured_json_parse` 的 `fallback_json` 要转义 |

## 3. 节点条目

### 3.1 键

| 键 | 必填 | 说明 |
| --- | --- | --- |
| `type` | 是 | 节点类型（3.2），例如 `llm_generate` |
| `name` | 是 | 节点名，方案内唯一。连线、`depends_on`、控制命令都用它定位节点。不能是 `input`、`output`（4.1 的保留名），不能含 `.` |
| `params` | 否 | 节点参数；省略表示全部使用默认值，只写与默认值不同的项 |
| `inputs` | 视节点而定 | 输入端口到数据来源的映射（第 4 节） |
| `depends_on` | 否 | 只表达先后顺序、没有数据传递的依赖（不变） |

- 条目只允许这五个键；`id`、`node_type`、`config`、`outputs` 删除。
- `pipeline` 是数组，理由与 `io`、`models` 相同：JSON 对象的重复键会被静默覆盖；数组保留作者写的顺序。
- `name` 的约定：snake_case，写这一步做什么，例如 `chunk_docs`、`embed_query`、`retrieve`、`generate_answer`。只是约定，除上面两条外不校验格式。

### 3.2 节点类型名

| 现名 | 改为 | 现名 | 改为 |
| --- | --- | --- | --- |
| LlmGenerateNode | `llm_generate` | TextTemplateNode | `text_template` |
| PromptGuidedLlmNode | `prompt_guided_llm` | TextRuleMatchNode | `text_rule_match` |
| TextEmbeddingNode | `text_embedding` | StructuredJsonParseNode | `structured_json_parse` |
| TextRerankNode | `text_rerank` | TextCorpusSourceNode | `text_corpus_source` |
| VectorTopKNode | `vector_top_k` | AsrTranscribeNode | `asr_transcribe` |
| TextChunkNode | `text_chunk` | OcrDetectNode | `ocr_detect` |

- 注册写法：`REGISTER_FUNCTION_NODE(llm_generate, Spec())`。宏的参数就是节点类型名，作者仍只写一个名字。
- 源文件名不变，本来就是 `<类型名>_node.cpp`。
- 脚手架 `tools/scaffold_custom_node.py` 改为接受 snake_case 名字，不再自动补 `Node` 后缀。
- 测试中自定义的节点类型名也改为 snake_case。

### 3.3 诊断

| 情况 | 诊断码 | 路径 |
| --- | --- | --- |
| 条目缺键、类型错误、未知键 | `MISSING_FIELD`、`FIELD_TYPE`、`UNKNOWN_FIELD`（不变） | `/pipeline/<j>/<键>` |
| 节点类型未注册 | `UNKNOWN_NODE_TYPE`（不变），附相近的类型名 | `/pipeline/<j>/type` |
| `name` 重复 | `DUPLICATE_NODE_NAME`（原 `DUPLICATE_NODE_ID`） | 后出现的一项，`/pipeline/<j>/name` |
| `name` 为保留名或含 `.` | `INVALID_NODE_NAME`（新增） | `/pipeline/<j>/name` |
| 参数未知、缺少、类型错误、超出范围、不在枚举内 | `UNKNOWN_CONFIG_FIELD`、`MISSING_CONFIG_FIELD`、`CONFIG_FIELD_TYPE`、`CONFIG_FIELD_RANGE`、`CONFIG_FIELD_ENUM`（不变） | `/pipeline/<j>/params/<参数>`；映射、数组中的元素继续往下写键名或下标 |
| 参数的 `Prepare`/`Validate` 失败 | `INVALID_COMBINATION`（不变） | `/pipeline/<j>/params` |
| `bind_model` 指向不存在的模型、类别不符 | `UNKNOWN_MODEL_REFERENCE`、`MODEL_TYPE_MISMATCH`（见模型设计） | `/pipeline/<j>/params/bind_model` |
| `depends_on` 指向不存在的节点、重复 | `INVALID_DEPENDENCY`、`DUPLICATE_DEPENDENCY`（不变） | `/pipeline/<j>/depends_on/<k>` |

连线的诊断见 4.4。

## 4. 连线

### 4.1 规则

| 规则 | 说明 |
| --- | --- |
| 输出 | 节点不写 `outputs`。每个输出端口都可以用 `节点名.端口名` 引用 |
| 输入 | `inputs` 写 `"输入端口": "来源"`。来源是 `节点名.端口名`，或 `input.端口名`（输入项发布的数据，4.2） |
| 回包 | `io` 的输出项写 `inputs`：`"converter 端口": "来源"`，来源写法同上 |
| 保留名 | `input`、`output` 表示方案的输入和输出，不能用作节点名 |
| 推导 | 校验器按引用连边，排出执行顺序，检查两端的数据类型、条数关系、溯源和生命周期，与现在相同 |
| 发布 | 只发布被引用的输出。没被引用的输出不占内存，也不会与别的节点冲突 |
| 先后约束 | `depends_on` 保留，只用于没有数据传递的情况 |

内部仍使用 Blackboard。校验器为每个被引用的输出生成内部数据名 `节点名.端口名`（输入项为 `input.端口名`），节点与 converter 按计划中的数据名读写；运行时不变。`plan --explain` 和运行追踪里显示的就是这个名字。

引用写法中只有一个 `.`：节点名不能含 `.`，端口名本来就是标识符。

### 4.2 io 的端口

- converter 的端口改为逻辑名，在各自登记内声明（附录 B）。端口名取它所读写的结构体成员，多个成员合成一个端口时取它的含义（例如 `doc_out` 的 `intent` 写入 `intent_name` 和 `confidence`）。
- 输入项发布的数据用 `input.端口名` 引用；多个输入项的端口不能重名（I/O 设计 5.5）。
- 输出项的 `inputs` 必须连接该 converter 的全部必填端口，可以引用节点输出，也可以直接引用 `input.端口名`。
- converter 读写数据时，通过计划给出的实际数据名访问，与节点的 `BoundInput`/`BoundOutput` 相同，不再使用代码里的常量：
  - 端口声明改为 `OutputPort<TextBatch>("doc_text")`、`RequiredInputPort<TextBatch>("answer_text")` 这样的写法；
  - `InputDecodeOptions`、`OutputEncodeOptions` 新增 `Port("doc_text")`，返回该端口在计划中的实际数据名（输入项为 `input.doc_text`，输出项为所连来源的 `节点名.端口名`），converter 用它读写 `AlgContext`；
  - 输入项中没被引用的端口，`Port` 返回空字符串，发布辅助函数 `PublishContextValue` 遇到空名字时跳过，与节点"只发布被引用的输出"一致。
- 删除 `include/adapter/biz_blackboard_keys.h`，端口名常量移到各 converter 文件内。`include/adapter/biz_input_constraints.h` 改名为 `include/adapter/input_limits.h`，命名空间 `biz_input` 改为 `input_limits`，内容不变。
- `rerank_in` 不再发布 `rerank_pairs`（6.2）。

### 4.3 生命周期跟随输入

- 计划中，每个输入端口都带上所连数据的生命周期（request / session）。`BindingFacts` 新增 `InputLifetime(端口)`，节点在 `Prepare` 中据此决定行为，结果存进参数结构体中不对应配置项的成员。
- `PortFlow` 删除 `lifetime_config_field`，改为声明"输出的生命周期跟随某个输入"：`PortFlow{"1:1", "preserve", FollowLifetime("text")}`。校验器按所连数据推出输出的生命周期，下游的生命周期检查照常进行。
- 被跟随的输入端口保持默认的 `request`，即 request、session 两种数据都能接（现有的兼容规则：生产者的生命周期不短于消费者）。
- 用到它的节点见 6.2：`text_embedding` 输入是会话数据时只算一次并缓存；`vector_top_k` 候选是会话数据时，所有请求共用这批候选。

### 4.4 诊断

| 情况 | 诊断码 | 路径 |
| --- | --- | --- |
| 必填输入端口没有连接 | `MISSING_INPUT_PRODUCER`（不变） | `/pipeline/<j>/inputs` |
| 输出 converter 的必填端口没有连接 | `MISSING_OUTPUT_PRODUCER`（I/O 设计中的改名） | `/io/output/<i>/inputs` |
| 来源写法错误（不是"名字.端口"） | `FIELD_TYPE` | `/pipeline/<j>/inputs/<端口>` 或 `/io/output/<i>/inputs/<端口>` |
| 来源的节点不存在 | `UNKNOWN_NODE_REFERENCE`（新增），附相近的节点名 | 同上 |
| 来源的端口不存在 | `UNKNOWN_PORT_REFERENCE`（新增），附该节点或输入项的输出端口 | 同上 |
| 两端数据类型不同 | `PORT_TYPE_MISMATCH`（新增，与已有的 `PORT_CARDINALITY_MISMATCH` 等对称） | 同上 |
| 条数关系、溯源、生命周期不兼容 | `PORT_CARDINALITY_MISMATCH`、`PORT_PROVENANCE_MISMATCH`、`PORT_LIFETIME_MISMATCH`（不变） | 同上 |
| 节点未知的输入端口 | `UNKNOWN_FIELD` | `/pipeline/<j>/inputs/<端口>` |
| 存在环 | `DAG_CYCLE`（不变） | `/pipeline` |

- 删除 `PARALLEL_WRITE_CONFLICT`：输出的内部数据名以节点名开头，不同节点不可能写同一个数据名。
- `DUPLICATE_PORT_PRODUCER` 只剩多个输入项发布同名端口这一种情况（I/O 设计 5.5）。
- 原来"没有唯一的生产者"一类问题都报 `MISSING_INPUT_PRODUCER`，现在按原因区分：引用不存在、端口不存在、类型不同。
- 修复建议（修复原因定义在 `include/core/remediation_cause.h`）：
  - 新增 `unknown_node_reference`，候选为相近的节点名；新增 `unknown_port_reference`，候选为该来源的输出端口；
  - `PORT_TYPE_MISMATCH` 沿用现有的 `port_type_mismatch`；`MISSING_INPUT_PRODUCER` 沿用现有的 `no_compatible_input_source`，候选改为类型相同的输出（`节点名.端口名` 形式）；
  - `unknown_dependency` 的候选改为节点名。

### 4.5 控制命令与命令行编辑

- 控制命令的定向信封：`{"$edgeflow_control":1,"node_id":"…","payload":{…}}` 改为 `{"$edgeflow_control":1,"node":"<节点名>","payload":{…}}`。不带信封时仍广播给支持该命令的节点（不变）。
- 命令行 `edit` 的操作：
  - `add_node`：`{"kind","type","name","params"}`；不给 `name` 时用节点类型名，重名时加 `_2`、`_3`；
  - `remove_node`：`{"kind","node"}`；
  - `rename_node`：`{"kind","node","new_name"}`，同时改写所有引用它的 `inputs`、`depends_on` 和输出项的 `inputs`；
  - `connect`、`disconnect`：`{"kind","source","target"}`，端点为 `{"node","port"}`；方案的输入、输出用保留名 `input`、`output`，取代 `$ingress`、`$egress`；
  - `add_dependency`、`remove_dependency`：`{"kind","node","depends_on"}`。
- 校验报告、修复建议中的边界伪节点同样改为 `input`、`output`。现在校验器混用 `$io_input`、`$io_output`、`$ingress`、`$egress`，在这里一次统一（I/O 设计不改它们）。

## 5. 参数声明

### 5.1 映射、数组与 JSON 值

`Field` 支持的成员类型（`std::optional<T>` 见模型设计 5.3）：

| 成员类型 | 配置中的写法 | Catalog 中的 `type` |
| --- | --- | --- |
| `std::string`、`bool`、`int`、`int64_t`、`float`、`double` | 标量（不变） | `string`、`boolean`、`integer`、`number` |
| `std::vector<T>` | 数组 | `array` |
| `std::map<std::string, T>` | 对象，键由使用者自定 | `map` |
| `nlohmann::json` | 任意 JSON 值，不能为 null | `json` |
| 结构体 `T` | 对象，只允许 `T` 声明的键。只能作为数组、映射的元素，用 `.Items(Parameters<T>)` 声明 | `object` |

- `T` 可以是表中任一类型，也可以嵌套，例如 `std::map<std::string, std::vector<std::string>>`。
- `Range`、`Enum` 用在数组、映射上时，约束其中的标量元素，例如 `field_types` 的每个值只能是 5 个类型名之一（6.4）。
- 元素是结构体时，用 `.Items(Parameters<T>)` 声明元素的字段：

```cpp
struct Endpoint {
  std::string prompt;
  std::vector<TextTemplateToken> parts;  // Prepare 的产出
};
auto EndpointParameters() {
  return Parameters<Endpoint>({
      Field("prompt", &Endpoint::prompt).Default("{{input}}").Description("…")})
      .Prepare(&CompilePrompt);
}
// 在节点参数中：
Field("endpoints", &Params::endpoints).Required().Items(EndpointParameters())
```

- 每个元素按 `T` 的声明校验、补默认值，再执行 `T` 的 `Prepare` 和 `Validate`。
- 诊断：
  - 元素的类型错误、未知键、缺少必填键、超出范围、不在枚举内，诊断码与单个值相同，路径继续往下写键名或下标，例如 `/pipeline/<j>/params/endpoints/time/prompt`、`/pipeline/<j>/params/rules/3/pattern`。为此 `ConfigFieldValidationError` 的 `field_name` 改为相对路径 `path`；
  - 元素的 `Prepare`、`Validate` 失败报 `INVALID_COMBINATION`，路径为该参数（如 `/pipeline/<j>/params/rules`），消息写明键名或下标。
- 定义：`ConfigFieldDefinition` 新增 `items`（数组、映射的元素定义）和 `fields`（结构体元素的字段）；`ConfigValueKind` 新增 `kMap`、`kJson`，`kObject` 只用于结构体元素。Catalog 的 `config_fields` 按此导出元素，控制命令的参数格式也由它生成（5.6）。
- 顺序：映射按键名的字典序处理，不保留书写顺序，与现在 `nlohmann::json` 的对象相同。需要按书写顺序处理时用数组。

### 5.2 共享参数组：`Include`

```cpp
Parameters<Params>({Field(...), ...})
    .Include(&Params::generation, GenerateParameters());
```

- 被并入的参数在 JSON 中与自有参数平铺在同一层，值写进指定成员。
- 两组重名时，构造 Spec 就报错。
- 被并入组的 `Prepare`、`Validate` 先于本组执行。
- 取代 `GenerateParameters` 的第二个重载、`NonDeduced` 技巧和生成参数对 `WithParser` 的使用。

### 5.3 生成参数

```cpp
// include/nodes/generate_parameters.h（替换 generate_options_config.h）
inline Parameters<GenerateOptions> GenerateParameters() {
  const GenerateOptions d;  // 默认值只在 GenerateOptions 中写一次
  return Parameters<GenerateOptions>({
      Field("max_tokens", &GenerateOptions::max_tokens).Default(d.max_tokens).Range(1, 32768)
          .Description("每条输入最多生成的 token 数，不含提示词；还受模型上下文容量限制"),
      Field("temperature", &GenerateOptions::temperature).Default(d.temperature).Range(0, 2)
          .Description("采样温度；0 为贪心生成"),
      // top_k、top_p、repetition_penalty、stop_words 同样写法；
      // system_prompt、random_seed 由模型设计阶段 2 加入（模型设计 8.1）
  }).Validate(&RejectEmptyStopWords);
}
```

- 所有 LLM 节点的同名参数默认值一致：`max_tokens` 统一为 128，不再由调用方传入。`prompt_guided_llm` 原来的 512 不再保留，方案中它都写了 `max_tokens`。
- `temperature` 等其他默认值不变。
- 只有生成参数的节点写 `GenerateParameters()`；还有自有参数时用 `Include`（5.2）。
- 删除 `GenerateOptionsFields`、`ParseGenerateOptions`。

### 5.4 `bind_model` 的说明由框架生成

`Model("generator", "bind_model", &Models::generator)` 不再接收说明文字。Catalog 中的说明由框架按槽位的模型类别生成，例如"引用 `models[].name`；所选模型的类别必须是 llm"。模型格式切换（模型设计阶段 2）之前，生成的文字仍写 `models[].model_id`。

### 5.5 删除的写法

文本处理类节点改用 `Field` 和字段控制命令之后（6.4），删除：
- `ConfigFieldDefinition` 的 8 元组构造函数；
- `ConfigParser`（原 `NodeConfigParser`，I/O 设计阶段 0 改名）和 `Parameters::WithParser`；
- `NodeSpec::WithControl`（手写 schema 加更新函数的控制命令，5.6）。

此后节点、converter、模型、后端都只有一种参数写法，节点只有一种控制命令写法。

### 5.6 控制命令

控制命令只有一种写法：`WithControls({ReplaceFields(命令号, "命令名", {"参数", ...})})`。

| 项目 | 规则 |
| --- | --- |
| 格式 | payload 是对象，只允许列出的参数。每个参数的格式由它的声明生成：类型、范围、枚举，以及数组、映射的元素。与配置使用同一套校验 |
| 语义 | 至少给出一个受控参数。给出的参数整体替换（数组、映射也整体替换），没给出的保持不变。替换后重新执行 `Prepare`、`Validate`，失败时保留原参数 |
| 限制 | 受控参数可以是任何类型；`bind_model` 不能受控（不变） |
| 命令号 | 不变：`update_rules` 为 1，`update_prompt` 为 2。Operator 的 `kUpdateRules`、`kSwitchPrompt`、`kJson` 与它们的对应关系不变 |

- 原来要求 payload 给出全部受控参数，改为至少一个。只有一个受控参数的命令（如 starter 的 `set_prefix`）行为不变。
- 由声明生成 JSON schema 的函数放在 `contracts/config_schema.h`（`ConfigFieldJsonSchema`），控制命令使用它；`export-schema` 以后也改用它（第 9 节）。
- 删除 `NodeSpec::WithControl`。以后出现不能表达为"替换参数"的命令时，再新增写法。

## 6. 各类节点

### 6.1 LLM 生成：`llm_generate`

`llm_generate`（原 LlmGenerateNode）改为配置一个或多个 endpoint：一个 endpoint 就是单次生成，多个就是对同一输入问多个问题。

```json
{ "type": "llm_generate", "name": "extract",
  "params": {
    "bind_model": "entity_llm",
    "system_prompt": "你是中文信息抽取助手，只输出结果本身。",
    "max_tokens": 64,
    "temperature": 0.1,
    "endpoints": {
      "time":  { "prompt": "提取下面这句话中的时间描述，没有就输出“无”。\n{{input}}" },
      "place": { "prompt": "提取下面这句话中的地点，没有就输出“无”。\n{{input}}" },
      "event": { "prompt": "用一个短语概括下面这句话要做的事。\n{{input}}" } } },
  "inputs": { "input": "input.sentence_text" } }
```

输入"明天下午三点在北京开会"，`document` 输出一条：`{"event": "开会", "place": "北京", "time": "明天下午三点"}`。

| 项目 | 规则 |
| --- | --- |
| `endpoints` | 必填，至少一个。键是 endpoint 名，值是对象 |
| endpoint 的 `prompt` | 提问模板，`{{input}}` 处填入输入文本；默认 `{{input}}`，即原样发送。只能使用 `{{input}}` 这一个变量 |
| 输入 | `input`（TextBatch，必填）。原名 `prompt`：提示词现在写在 endpoint 里，输入只是材料。变量名与端口名相同 |
| 执行 | 每个 endpoint 渲染一批提示词，调用一次模型；输入 N 条，输出 N 条，`req_id`、`sub_id` 不变 |
| 生成参数 | 节点级，所有 endpoint 共用（`Include(GenerateParameters())`） |
| 输出 `document` | StructuredDocumentBatch。每条一个对象 `{endpoint 名: 回答}`，单个 endpoint 也一样；回答是模型返回的原文。对象的键按字母序排列 |
| 输出 `text` | TextBatch。只有一个 endpoint 时是回答本身，与现在的输出完全相同；有多个时是 `document` 的 JSON 文本 |
| 失败 | 任一 endpoint 调用失败，节点失败，与其他节点一致 |
| 对话标记 | 不写在配置里。ChatML 等标记由模型添加（`qwen_causal_lm` 已经这样做；kite 后端收到的提示词已经由模型加好标记，自己不套模板），换模型时不用改提示词 |

提示词写在哪里：
- `system_prompt`：角色，本节点所有 endpoint 共用；
- endpoint 的 `prompt`：要问的问题；
- `text_template`：把问题、检索片段、规则命中、OCR 文本等多个来源拼成一段材料。

以后的扩展都是新增，现有配置不用改：
- 每个 endpoint 单独设置 `max_tokens`、`thinking`、`system_prompt`：在 endpoint 对象中增加键；
- 某个 endpoint 输出 JSON 值（例如人名列表）：增加 endpoint 选项，把回答解析为 JSON；
- 需要把某个 endpoint 的结果单独接给下游：增加按 endpoint 生成的输出端口。这需要 Core 支持由参数决定的端口，目前没有需求，不做。

`prompt_guided_llm` 保留，作为自定义节点的写法示例（Demo 的 custom 方案在用），参数声明改用 `Include(GenerateParameters())`。它的 `prompt_prefix` 已在模型设计阶段 2 删除。

### 6.2 向量与检索

| 节点 | 改动 |
| --- | --- |
| `text_embedding` | 删除 `normalize`（移到向量模型，模型设计 8.4）；删除 `lifetime`：输入是会话数据时，在会话内只算一次并缓存，输出也是会话数据，否则每次请求计算（4.3）。会话缓存的键为模型版本加输入内容 |
| `vector_top_k` | 删除 `candidate_scope`：候选是会话数据时，所有请求共用这批候选（候选的 `req_id` 必须为 0，与现在相同）；否则只在本请求的候选中找。`candidate_texts` 改为必填。参数只剩 `top_k`（默认 1）、`min_score`（默认 0）、`metric`（默认 `cosine`）。模型开启归一化（默认开启）时，`cosine` 与 `dot_product` 的排序相同 |
| `text_rerank` | 输入只保留 `queries` 与 `candidates`，都改为必填；删除 `pairs`、`candidate_texts` 和"三组只能接一组"的约束。参数只有 `top_k`（每个请求最多保留几个，默认 1）。以后需要不经过向量检索、直接对一组文本重排时，再新增一个输入 |

检索链路的写法：

```json
{ "type": "text_corpus_source", "name": "policy_corpus", "params": { "corpus": [ "…" ] } },
{ "type": "text_embedding", "name": "embed_policies", "params": { "bind_model": "embed_model" },
  "inputs": { "text": "policy_corpus.corpus" } },
{ "type": "text_embedding", "name": "embed_query", "params": { "bind_model": "embed_model" },
  "inputs": { "text": "input.user_text" } },
{ "type": "vector_top_k", "name": "retrieve_policies", "params": { "top_k": 3 },
  "inputs": { "queries": "embed_query.embedding", "candidates": "embed_policies.embedding",
              "candidate_texts": "policy_corpus.corpus" } },
{ "type": "text_rerank", "name": "rerank_policies", "params": { "bind_model": "rerank_model" },
  "inputs": { "queries": "input.user_text", "candidates": "retrieve_policies.ranked" } }
```

`policy_corpus` 是会话数据，所以 `embed_policies` 每个会话只算一次，`retrieve_policies` 的所有请求共用这批候选。

### 6.3 语音与图像

- `asr_transcribe`：新增 `language`，见模型设计 8.2；其余不变。
- `ocr_detect`：只改类型名。

### 6.4 文本处理类

`text_template`、`text_rule_match`、`structured_json_parse`、`text_corpus_source`、`text_chunk` 是不绑模型的通用节点，只按自身逻辑处理数据。写法与其他节点一致：参数只用 `Field` 声明，控制命令用 5.6 的字段命令，数据靠连线引用。

删除的依据与 6.2 相同，只删三类：
1. 同一内容的第二种写法；
2. 没有生产者、任何方案都接不上的输入，以及随之失去作用的参数；
3. 能用连线或 `depends_on` 表达的。

其余行为参数，即使方案中没写也保留，与 6.2 保留 `metric` 一样。

| 节点 | 输入 | 参数（默认值） | 控制命令 | 删除 |
| --- | --- | --- | --- | --- |
| `text_template` | `primary`（TextBatch，逐条）；`context`（RankedTextBatch）、`matches`（RuleMatchBatch）、`document`（OcrDocumentBatch），这三个按请求聚合。都可选，至少连接一个（不变） | `template`（`{{primary}}`）、`separator`（`"\n"`）、`max_length`（65536）、`overflow_policy`（`fail`，可选 `truncate`） | `update_prompt`：替换 `template` | 输入 `context_text`、`document_text`、`attributes`；参数 `values`、`allow_dynamic_attributes`、`missing_variable_policy` |
| `text_rule_match` | `text`（TextBatch） | `categories`（映射，默认空）、`rules`（对象数组，默认空）、`default_category`（`""`）、`default_score`（1，0–1） | `update_rules`：替换 `categories`、`rules` 中给出的项 | 无 |
| `structured_json_parse` | `text`（TextBatch） | `fallback`（JSON 值，默认 `{}`）、`extract_json_block`（true）、`required_fields`（字符串数组，默认空）、`field_types`（映射，默认空）、`failure_policy`（`configured_fallback`，可选 `fail`、`emit_diagnostic`） | 无 | `fallback_json` 改为 `fallback` |
| `text_corpus_source` | 无 | `corpus`（字符串数组，必填） | 无 | 输入 `trigger` |
| `text_chunk` | `text`（TextBatch） | `chunk_size`（100）、`overlap`（0） | 无 | 无。已经用 `Field`，只改类型名 |

#### `text_template`

- 删除的输入：
  - `context_text`、`document_text`：与 `context`、`document` 内容相同、类型不同，方案都没用；
  - `attributes`：它的类型 `TextAttributesBatch` 没有任何节点或 converter 产出。删除这个类型（`include/core/common_contracts.h`）。

  以后需要把普通文本按请求拼进模板时，再新增一个输入。
- 删除的参数：
  - `values`（静态变量）：直接写进模板即可；
  - `allow_dynamic_attributes`：只为 `attributes` 服务；
  - `missing_variable_policy`：删掉前两项后，变量只能来自已连接的输入，运行时不会缺失。
- 模板变量只能是已连接的输入端口名。未知变量、引用没连接的输入，都在 `Prepare` 中报错（`INVALID_COMBINATION`）。删除 `node_error::text_template::kMissingVariable`。
- 渲染规则不变：
  - 同一请求的多条 `context`、`document` 用 `separator` 拼接；
  - `matches` 写成 `类别 (关键词)`，用 `, ` 拼接；
  - 输入连接了、但某个请求没有数据时，该变量为空字符串。
- `update_prompt` 只替换 `template`，替换后重新编译模板、检查变量；失败时保留原模板。
- 接入层把 Operator 的 `kSwitchPrompt` 转成 `{"template": …}`。平台结构中的 `prompt_id` 仍按现有规则检查长度，但不再转发：节点只保存它，从不使用。

#### `text_rule_match`

```json
{ "type": "text_rule_match", "name": "parse_command",
  "params": {
    "default_category": "GENERAL_VOICE_CMD",
    "categories": { "MUSIC_PLAY": ["播放", "音乐", "我想听"] },
    "rules": [ { "id": "nav_dest", "strategy": "regex", "category": "NAVIGATION",
                 "pattern": "导航到(?<destination>.+?)(?:，|,|。|\\s|$)",
                 "constants": { "avoid_toll": false } } ] },
  "inputs": { "text": "transcribe.text" } }
```

- `categories` 与 `rules` 都保留。它们不是同一功能的两种写法：
  - `categories` 是关键词表：类别 → 关键词数组。文本含任一关键词即命中该类别，分数为 1；
  - `rules` 是逐条规则：按 `contains`、`exact` 或 `regex` 匹配，可以用正则的命名分组抽取槽位、用 `constants` 附加固定槽位、设置分数。

  方案中 16 个节点都写了关键词表，其中语音意图的 mock 方案还用规则抽取目的地、温度等槽位。
- 规则元素用 `.Items(RuleParameters())` 声明：
  - `pattern`：必填；
  - `id`：`""`；
  - `strategy`：`contains`，可选 `exact`、`regex`；
  - `category`：`""`；
  - `score`：1，范围 0–1；
  - `constants`：映射，值为任意 JSON，默认空。

  元素的 `Prepare` 编译正则，结果放在不对应配置项的成员里；正则写错时报 `INVALID_COMBINATION`，消息写明下标和 `id`。
- 匹配顺序不变：先按类别名的字典序查关键词表，再按数组顺序查规则；第一个带类别的命中决定结果的类别。
- `update_rules` 的行为与现在相同：给出的 `categories`、`rules` 整体替换，没给出的不变。

#### `structured_json_parse`

```json
{ "type": "structured_json_parse", "name": "parse_verdict",
  "params": {
    "fallback": { "verdict": "合规", "risk_level": "SAFE", "risk_score": 0.1, "suggestion": "无违规内容" },
    "required_fields": ["risk_level", "risk_score"],
    "field_types": { "risk_level": "string", "risk_score": "number" } },
  "inputs": { "text": "generate_verdict.text" } }
```

- `fallback_json` 改为 `fallback`：原来是写在字符串里、需要转义的 JSON，现在直接写 JSON 值。
  - 使用备用值时，输出的文本是它的紧凑序列化；
  - 现有业务的输出 converter 都拒绝使用了备用值的结果，所以回包不变。
- `field_types` 的值只能是 `string`、`number`、`boolean`、`object`、`array`（`Enum` 作用于映射的值）。
- `Validate`：`required_fields` 的元素不能为空；`failure_policy` 不是 `fail` 时，`fallback` 必须满足 `required_fields` 和 `field_types`（不变）。
- 默认值只写在声明里。删除三处默认值常量和 `Load()` 中的手写读取。

#### `text_corpus_source`

- `corpus` 改为必填，可以是空数组；元素类型由 `Field` 检查，删除手写检查。
- 删除可选输入 `trigger`：它不传数据、只为排顺序，与 `depends_on` 重复。节点没有输入，输出 `corpus` 仍是会话数据。

#### 迁移

本节在节点设计阶段 1 实施，此时节点条目仍是旧格式（实施总说明第 5 步）：
- 方案中只有 `fallback_json` 需要改写：
  - `configs/` 的 dialogue_audit_default、entity_extract_default，以及 `demo/fixtures/mock/` 的 dialogue_audit、entity_extract，改为 `fallback`，值写成 JSON；
  - `demo/fixtures/mock/` 的 ocr_invoice_qa 写的是 `"{}"`，等于默认值，删除；
  - `tests/fixtures/pipelines/` 和测试中内嵌的配置同样处理。
- 其余删除项，方案中都没有使用，只改测试。

## 7. 现有配置的迁移规则

在本地一次性迁移；迁移脚本不提交（CONTRIBUTING §3）。

1. 节点条目：
   - `node_type` 改为 `type`，按 3.2 改名；
   - `id` 改为 `name`，按用途重新命名。各方案的对照在 PR 描述中列出，例如文档问答为 `chunk_docs`、`embed_query`、`embed_chunks`、`retrieve`、`build_prompt`、`match_intent`、`generate_answer`；
   - `config` 改为 `params`，为空时省略；
   - 删除 `outputs`；
   - 键的顺序：`type`、`name`、`params`、`inputs`、`depends_on`。
2. 连线：
   - 每个 `inputs` 的值，从数据名改为写出该数据的节点 `名字.端口名`；由输入 converter 发布的数据改为 `input.端口名`（附录 B）；
   - 输出项新增 `inputs`，把 converter 的每个端口连到原来提供该数据名的节点。
3. LLM 节点：
   - 输入端口 `prompt` 改为 `input`；
   - 新增一个 endpoint，按输出的用途命名，提示词保持不变（`{}`）：文档问答 `answer`，对话审核 `verdict`，实体抽取 `entities`，翻译 `translation`，发票问答 `invoice`。
4. 参数：
   - 删除 `lifetime`、`candidate_scope`；
   - 等于默认值的参数一律删除，与模型设计相同。`configs/` 中的清单见附录 C，`demo/fixtures/mock/` 按同一规则处理；测试夹具按测试需要决定。
5. `text_rerank`：方案都只用 `queries` 加 `candidates`，不用改。

需要迁移的文件：
- `configs/*.json`（17 个）；
- `demo/fixtures/mock/*.json`（9 个）；
- `tests/fixtures/pipelines/` 下的 JSON（含 `validation/invalid_pipeline_cases.json`）；
- 资源清单中的方案模板（如有节点条目）；
- C++、Python、JS 测试中内嵌的配置文本；
- 文档里的配置示例。

## 8. 运行链路

```text
Create（每个句柄一次）
  1. .conf → pipe_path → Pipeline JSON（不变）
  2. 拆出 io（I/O 设计）；解析模型文件（模型设计）
  3. Core 校验 models（模型设计）
  4. Core 校验节点条目：类型、名字、参数                                       【改】
  5. Core 解析引用并连边：节点 inputs、输出项 inputs；推导输出的生命周期；
     检查类型、条数、溯源、生命周期；拓扑排序                                     【改】
  6. 为被引用的输出生成内部数据名；未被引用的输出不进入计划                       【新】
  7. 物化模型；创建节点：解析参数（Prepare 可读取输入的生命周期）→ 绑定模型
     → ValidateModels                                                           【改】
Process（每批一次）
  1. 输入项按计划中的数据名发布                                                 【改】
  2. 按拓扑顺序执行节点；节点只发布被引用的输出                                 【改】
  3. 输出项按计划中的数据名读取，写入宿主结构体                                 【改】
```

## 9. 对外接口与工具

| 工具 | 变化 |
| --- | --- |
| Catalog JSON | `nodes[].node_type` 的值改为 snake_case；`config_fields` 的 `type` 新增 `map`、`json`，数组和映射带 `items`，结构体元素带 `fields`（5.1）；`control_commands[]` 的 payload schema 由参数声明生成（5.6）；`model_dependencies[]` 的 `config_field` 不变；`input_converters[]`/`output_converters[]` 的端口改为逻辑名 |
| `describe-node` | 参数为 snake_case 类型名 |
| `validate` / `plan` | 诊断按 3.3、4.4；`plan --explain` 显示每条边 `来源 → 节点.端口` |
| `init` | 生成的草稿为新格式 |
| `edit` | 按 4.5 |
| `export-schema` | 节点条目：`type` 为节点类型枚举，`name`、`params`、`inputs`、`depends_on`；`params` 按 `type` 用 if/then 分支，每个参数的 schema 用 `ConfigFieldJsonSchema` 生成（5.6）；`inputs` 的值为 `^[^.]+\.[^.]+$` 形式的字符串；输出项增加 `inputs` |
| 修复建议（`src/cli/pipeline_remediation.cpp`） | 按 4.4 |
| `alg_show` | 显示节点名与类型；边显示为 `来源 → 节点.端口` |
| Studio | 节点表单为类型、名字、参数；连线即引用：画一条边就写一条 `inputs`，删除"数据名"概念；改名时同步改写引用；输出项的端口作为画布上的输出边界 |
| `tools/verify_selection.py`、`tools/dev_recipe.py` | 读写节点条目、克隆方案时按新格式；按节点名而不是 ID 查找 |
| 控制命令 | 信封按 4.5 |

## 10. 实施阶段

| 阶段 | 内容 | 依赖 |
| --- | --- | --- |
| 1 | 参数声明统一：元素类型、`Include`、生成参数、`bind_model` 说明、控制命令由声明生成；文本处理类（6.4）；删除 8 元组、`ConfigParser`、`WithControl`。配置只改 `fallback_json`（6.4） | I/O 设计阶段 0 |
| 2 | 新的节点条目与连线（格式切换） | 本设计阶段 1；I/O 设计阶段 2；模型设计阶段 2 |

阶段 1 在模型设计阶段 2 之前合入，这样模型设计中新增的 `system_prompt`、`random_seed` 直接写成两行 `Field`。实施总说明把阶段 1 分为两步：第 4 步做机制（5.1–5.4、5.6），第 5 步做文本处理类与 5.5 的删除。

### 阶段 1：参数声明统一

| 文件 | 改动 | 步骤 |
| --- | --- | --- |
| `include/contracts/parameters.h`、`config_schema.h`、`config_schema_validation.h` | 5.1 的元素类型、`items`、`fields` 与元素的诊断路径；5.2 的 `Include`；5.6 的 `ConfigFieldJsonSchema` | 4 |
| `include/nodes/generate_parameters.h`（新增）、`include/nodes/generate_options_config.h`（删除） | 5.3 | 4 |
| `include/nodes/function_node.h`、`control_authoring.h` | `Model(...)` 去掉说明参数，按类别生成（5.4）；字段命令的格式由声明生成、至少给出一个参数（5.6） | 4 |
| `src/common_nodes/llm_generate_node.cpp`、`src/custom_nodes/prompt_guided_llm_node.cpp`、`dev_support/node_authoring/starter_llm_node.cpp` | 改用 `GenerateParameters()` / `Include`；本阶段仍是单个提示词 | 4 |
| `src/core/pipeline_validator.cpp`、`src/core/pipeline_catalog.cpp`、`src/cli/pipeline_remediation.cpp` | 元素的诊断路径；Catalog 导出 `items`、`fields`；修复建议处理新的参数类型 | 4 |
| `src/common_nodes/text_template_node.cpp`、`text_rule_match_node.cpp`、`structured_json_parse_node.cpp`、`text_corpus_source_node.cpp` | 6.4 | 5 |
| `include/core/common_contracts.h`、`include/nodes/node_error_codes.h` | 删除 `TextAttributesBatch`、`text_template::kMissingVariable`（6.4） | 5 |
| `src/adapter/operator/operator_control_registry.cpp` | `kSwitchPrompt` 不再转发 `prompt_id`（6.4） | 5 |
| `include/contracts/config_schema.h`、`parameters.h`、`include/nodes/function_node.h` | 5.5 的删除 | 5 |
| 方案 | 6.4 的迁移：`fallback_json` 改为 `fallback` | 5 |
| `tools/scaffold_custom_node.py`、生成的测试 | 新的生成参数写法 | 4 |
| 测试 | `tests/unit/nodes/test_parameter_binding.cpp`（元素类型、`Include`）、`test_function_node.cpp`（字段命令）、`test_common_nodes.cpp`、`tests/contract/catalog/*` | 4 |
| 测试 | `tests/unit/nodes/test_function_node.cpp`（删除 `WithControl`、`WithParser`、8 元组的用例）、`test_text_template_node.cpp`、`test_text_rule_match_node.cpp`、`test_structured_json_parse_node.cpp`、`test_text_corpus_source_node.cpp`、`test_common_nodes.cpp`、`tests/unit/core/test_node_base_contracts.cpp`、`test_definition_schema_validation.cpp`、`tests/integration/runtime/test_runtime_control_and_hot_swap.cpp`、`tests/integration/operator/test_operator_api.cpp`、`tests/integration/pipeline/test_pipeline_catalog_validator.cpp`、`tests/integration/demo/test_demo_runner.cpp` | 5 |
| 文档 | `doc/dev_guide/custom_node_concepts.md`、`first_control.md`、`doc/developer_guide.md`（链接了 `generate_options_config.h`）、`.agents/skills/edgeflow-node-developer`、`llm-edgeflow-developer-guide/references/capability-nodes.md`、`src/custom_nodes/README.md` | 4、5 |

**验收**：
- 方案文件只有 6.4 的 `fallback_json` 一处变化；
- `describe-node` 的参数名、类型、默认值和范围不变。例外：`prompt_guided_llm` 的 `max_tokens` 默认值改为 128；数组和映射参数多出元素说明；6.4 列出的删除与改名；
- 控制命令的 payload schema 由声明生成，`update_prompt`、`update_rules` 的行为与改造前相同；
- `git grep` 不再出现 8 元组构造、`ConfigParser`、`WithParser`、`WithControl`（单数）、`GenerateOptionsFields`、`TemplateControlSchema`、`RuleControlSchema`、`TextAttributesBatch`、`fallback_json`、`missing_variable_policy`、`allow_dynamic_attributes`；
- 过渡期门禁通过（实施总说明第 4 节）。

### 阶段 2：新的节点条目与连线

建议按下列顺序提交。中间提交尽量保持可编译；PR 最后必须通过门禁。

| 提交 | 内容 | 主要文件 |
| --- | --- | --- |
| C1 节点类型改名 | 3.2：注册名、测试中的类型名、脚手架 | `src/common_nodes/*`、`src/custom_nodes/*`、`dev_support/node_authoring/*`、`tools/scaffold_custom_node.py`、`tests/**` |
| C2 Core 条目与连线 | 3.1、3.3、第 4 节：条目解析、引用解析与连边、内部数据名、只发布被引用的输出、生命周期跟随输入（`PortFlow`、`BindingFacts`）、诊断码、控制信封、边界伪节点统一为 `input`/`output`；`node_id` 一类的代码名改为 `node_name` | `include/core/*`、`src/core/*`、`include/contracts/parameters.h`（`BindingFacts`）、`include/nodes/function_node.h`、`node_base.h`、`configuration_snapshot.h`、`tests/unit/core/*`、`tests/unit/nodes/*`、`tests/integration/pipeline/*` |
| C3 io 端口 | 4.2：converter 逻辑端口、输出项 `inputs`、按计划数据名读写；删除 `biz_blackboard_keys.h`；`input_limits.h`；`scripts/check_layer_isolation.sh` 的规则 4 由"下层不得包含 `adapter/biz_blackboard_keys.h`"改为"下层不得包含任何 `adapter/` 头文件"（现在没有违反），自检用例改为注入其他 `adapter/` 头文件 | `scripts/check_layer_isolation.sh`、`include/adapter/*`、`src/adapter/**`、`tests/unit/adapter/*`、`tests/contract/catalog/test_converter_contracts.cpp` |
| C4 节点 | 6.1 的 `llm_generate`；6.2 的三个检索节点 | `src/common_nodes/llm_generate_node.cpp`、`text_embedding_node.cpp`、`vector_top_k_node.cpp`、`text_rerank_node.cpp`、相关测试 |
| C5 命令行工具 | 第 9 节的命令行部分；4.5 的 `edit` | `src/cli/*`、`tests/RuntimeTests.cmake` |
| C6 Studio 与 Python 工具 | 第 9 节 | `tools/pipeline_studio/*`、`tools/verify_selection.py`、`tools/dev_recipe.py`、`tests/tooling/*` |
| C7 配置与测试迁移 | 第 7 节 | 第 7 节列出的文件；`tests/contract/*`、`tests/integration/*`、`tests/e2e/*` |
| C8 文档与规则 | 第 12 节；删除本文件 | 见第 12 节 |

**验收**：
- 8 个业务的 Operator 黄金测试只改配置路径和内容，期望值一律不改，全部通过；
- 改造前后分别运行 `alg_demo --suite smoke`，`results.jsonl` 的 `output` 字段逐条一致；
- 全部方案改为新格式并通过 `validate` 和 `plan`；kite 方案在 kite 构建中验证；
- 第 11 节的新增测试全部通过；
- 除 `doc/CHANGELOG.md` 的历史条目外，`git grep` 不再出现：Pipeline JSON 中节点条目的 `id`、`node_type`、`config`、`outputs` 键；大驼峰节点类型名；`biz_blackboard_keys.h`、`biz_input_constraints.h`、`lifetime_config_field`、`candidate_scope`、`PARALLEL_WRITE_CONFLICT`、`DUPLICATE_NODE_ID`、`$io_input`、`$io_output`、`$ingress`、`$egress`。不为这些旧名字编写"会被拒绝"的专门测试；
- 没有新增任何兼容分支、别名或迁移代码；
- 门禁通过；本地有真实权重时运行 `scripts/run_real_model_e2e.sh`。

## 11. 测试计划

| 范围 | 用例 | 阶段 |
| --- | --- | --- |
| 参数声明 | 映射与数组：元素类型错误、`.Items` 元素缺必填键、未知键、默认值、元素的 `Prepare`/`Validate`、嵌套（映射的值为数组）、`Range`/`Enum` 作用于元素；诊断路径带键名或下标；映射按键名排序；JSON 值参数接受对象、数组和标量，拒绝 null；Catalog 导出 `items`、`fields`。`Include`：平铺、重名时构造报错、被并入组先执行 | 1 |
| 生成参数 | 默认值与 `GenerateOptions` 一致；空的 `stop_words` 元素报错；两个 LLM 节点的 `max_tokens` 默认都是 128 | 1 |
| 控制命令 | payload schema 由声明生成，含映射、对象数组的元素；只给部分受控参数时其余不变；空 payload、未列出的参数、元素格式错误都被拒绝；替换后 `Prepare` 失败时保留原参数；`bind_model` 不能受控 | 1 |
| 文本处理类 | `text_template`：变量必须是已连接的输入，未知变量、未连接的输入在 `Prepare` 报错；`update_prompt` 替换模板，失败保留原模板；`kSwitchPrompt` 带 `prompt_id` 时照常生效。`text_rule_match`：`categories` 映射、`rules` 元素默认值、正则写错时报错并写明下标、`constants` 的各种 JSON 值、匹配顺序；`update_rules` 只替换给出的项。`structured_json_parse`：`fallback` 为对象、数组时输出与改造前一致；`field_types` 的值不在枚举内时报错；`fallback` 不满足字段检查时报错。`text_corpus_source`：不写 `corpus` 时报 `MISSING_CONFIG_FIELD`；没有输入时照常输出会话数据 | 1 |
| 节点条目 | 缺键、未知键、旧键（按未知字段处理）；未注册类型；`DUPLICATE_NODE_NAME`；`INVALID_NODE_NAME`（`input`、`output`、含 `.`）；省略 `params` 时使用默认值 | 2 |
| 连线 | 引用节点输出、引用 `input.端口`；`UNKNOWN_NODE_REFERENCE` 与相近名字建议；`UNKNOWN_PORT_REFERENCE`；`PORT_TYPE_MISMATCH`；必填输入未连接；输出项必填端口未连接；环；`depends_on`；未被引用的输出不发布；`plan --explain` 的边 | 2 |
| 生命周期 | 语料来自 `text_corpus_source` 时，`text_embedding` 每个会话只调用一次模型，输出为会话数据；输入是请求数据时每次请求计算；`vector_top_k` 对会话候选共用、对请求候选按请求查找；下游的生命周期检查 | 2 |
| `llm_generate` | 单个 endpoint 时 `text` 为回答本身；多个 endpoint 时 `document` 的键与配置一致、`text` 为 JSON 文本；输入 N 条输出 N 条且溯源不变；每个 endpoint 一次模型调用；未知模板变量报错；任一 endpoint 失败时节点失败；`entity_out` 输出多 endpoint 的结果 | 2 |
| 检索节点 | `candidate_texts` 未连接时报 `MISSING_INPUT_PRODUCER`；`text_rerank` 只接受 `queries` 加 `candidates` | 2 |
| io 端口 | 输出项 `inputs` 连接节点输出或 `input.端口`；converter 按计划数据名读写；converter 契约测试改为固定逻辑端口名 | 2 |
| 控制与编辑 | 定向信封按节点名定位；`edit` 的各操作，`rename_node` 同步改写引用；自动命名 | 2 |
| 工具 | catalog、describe-node、export-schema 能校验全部方案、修复建议、Studio 的 Python 与浏览器测试、verify_selection、dev_recipe | 2 |
| 集成 | Operator 黄金测试、全业务集成测试、Demo smoke 结果不变；有真实权重时运行 e2e | 2 |

## 12. 文档与规则更新

| 文件 | 改动 |
| --- | --- |
| `AGENTS.md` | "`PipelineValidator` alone derives data dependencies from explicit `inputs` / `outputs` bindings" 改为 "from explicit `inputs` references (`node.port`)" |
| `doc/dev_guide/first_control.md`、`doc/dev_guide/custom_node_concepts.md`、`src/custom_nodes/README.md`、`.agents/skills/edgeflow-node-developer`、`llm-edgeflow-developer-guide/references/capability-nodes.md`（阶段 1） | 参数的元素类型与 `Include`；控制命令只有字段命令，至少给出一个参数（5.6）；删除 `WithParser`、`WithControl` 及"组合使用时必须显式 `Prepare`"的说明；删除 `missing_variable_policy` 的说明（6.4） |
| `doc/architecture.md`、`doc/developer_guide.md`、`doc/architecture_flow.puml` | 节点条目、连线与内部数据名、生命周期跟随输入；运行 `./scripts/render_architecture_diagrams.sh --generate` 重新生成 SVG |
| `doc/dev_guide/custom_node_concepts.md`、`doc/dev_guide/first_control.md`、`doc/dev_guide/recipe_*.md`、`doc/dev_guide/business_onboarding.md` | 新的条目与连线写法；控制信封；生成参数与 `Include`；映射与对象数组参数 |
| `configs/README.md`、`doc/solutions/*.md`、`doc/VERIFIABLE_SELECTION.md` | 配置示例 |
| `src/custom_nodes/README.md`、`src/adapter/input/README.md`、`src/adapter/output/README.md` | 节点类型名、端口逻辑名 |
| `tools/pipeline_studio/README.md` | 连线即引用 |
| `.agents/skills/pipeline-composer`（含 `references/workflow.md`）、`edgeflow-solution-planner`、`json-prompt-solution`、`edgeflow-node-developer`、`edgeflow-adapter-developer`、`llm-edgeflow-developer-guide/references/orchestration.md`、`capability-nodes.md`、`integration.md` | 条目、连线、类型名、`llm_generate` 的 endpoints、提示词分工 |
| `doc/CHANGELOG.md` | 各阶段的用户可见变化 |

## 13. 风险与不做的事

| 项目 | 处理 |
| --- | --- |
| 改动面大：阶段 2 涉及全部方案、Core 校验器、接入层 converter、命令行、Studio | 按 C1–C8 提交；黄金测试期望值不改；配置只在阶段 2 切换一次 |
| 节点改名要同步改写引用 | 命令行 `rename_node` 和 Studio 一并改写；手工改错时报 `UNKNOWN_NODE_REFERENCE` 并给出建议 |
| 删除 `WithControl` | 生产代码中只有两个文本处理节点在用，改为字段命令后不再需要；以后出现不能表达为"替换参数"的命令时再新增（5.6） |
| 文本处理节点删除的输入和参数 | 方案中都没有使用；以后需要时作为新的输入、参数加回，现有配置不受影响（6.4） |
| 不做：每个 endpoint 一个输出端口 | 需要 Core、Catalog、Studio 支持由参数决定的端口；目前没有单独转发某个 endpoint 的需求（6.1） |
| 不做：节点名格式校验 | 只禁止保留名和 `.`；其余靠约定 |
| 不做：按请求选择 endpoint | 宿主请求中的 `endpoint` 字段（如翻译请求）目前不使用；需要时由输入 converter 发布，再由节点使用，属于新增 |

## 14. 已确认的决定

| 决定 | 内容 |
| --- | --- |
| 节点条目 | `type`（节点类型）、`name`（节点名）、`params`（可省略）、`inputs`、`depends_on`；去掉 `id`、`node_type`、`config`、`outputs`；`pipeline` 是数组 |
| 节点类型名 | snake_case，去掉 `Node` 后缀 |
| 节点名 | 按用途命名；连线、`depends_on`、控制命令都用它 |
| 连线 | `inputs` 写 `节点名.端口名` 或 `input.端口名`；输出项用 `inputs` 写明回包来源；DAG 由校验器自动推导；未被引用的输出不发布 |
| `bind_model` | 保留为必填的节点参数，引用模型的 `name`，不按类别自动绑定 |
| LLM 节点 | `llm_generate` 配一个或多个 endpoint；`document` 始终为 `{endpoint 名: 回答}`；`text` 在单个 endpoint 时为回答本身、多个时为 `document` 的 JSON 文本；`temperature` 默认值不变 |
| 提示词 | 角色写 `system_prompt`；问题写 endpoint 的 `prompt`；多个来源的材料用 `text_template` 拼接；对话标记由模型添加 |
| 业务后处理 | 写一个节点：调用模型后再处理（模型设计第 3 节） |
| 向量与检索 | `normalize` 移到模型；`lifetime`、`candidate_scope` 删除，由输入的生命周期决定；`candidate_texts` 必填；`text_rerank` 只保留 `queries` 与 `candidates` |
| 参数机制 | 只有 `Field`；支持数组、映射、结构体元素、JSON 值与 `Include`；删除 8 元组与手写解析 |
| 控制命令 | 只有字段命令，格式由参数声明生成；给出的参数整体替换，至少给出一个；删除 `WithControl` |
| 文本处理类 | 用户委托（6.4）：与其他节点同一写法、不绑模型；只删同一内容的第二种写法、接不上的输入和能用连线表达的项；`fallback` 直接写 JSON 值；`categories` 与 `rules` 都保留 |
| 端口与诊断码 | converter 逻辑端口名按附录 B；新增 `UNKNOWN_NODE_REFERENCE`、`UNKNOWN_PORT_REFERENCE`、`PORT_TYPE_MISMATCH`、`INVALID_NODE_NAME`；`DUPLICATE_NODE_ID` 改为 `DUPLICATE_NODE_NAME`；删除 `PARALLEL_WRITE_CONFLICT`（3.3、4.4） |
| 保留名与控制信封 | `input`、`output` 为保留名，取代各种边界伪节点名；定向信封的键为 `node`（4.5） |
| 默认值 | 配置中等于默认值的参数一律删除，只写与默认值不同的项（附录 C） |
| 迁移取值 | endpoint 名：`answer`、`verdict`、`entities`、`translation`、`invoice`；`max_tokens` 默认值统一为 128（5.3、第 7 节） |
| 不做兼容 | 不保留旧字段别名、不识别旧格式、不提交迁移代码；不为旧名字编写专门的拒绝测试 |

## 附录 A：节点类型名对照

见 3.2。

## 附录 B：converter 的逻辑端口

| 登记（`type/name`） | 方向 | 端口（类型） | 原数据名 |
| --- | --- | --- | --- |
| `audio_in/audio_asr_intent` | 输入 | `audio`（AudioPcmBatch） | `audio_inputs` |
| `audit_in/dialogue_audit` | 输入 | `user_text`、`channel_name`（TextBatch） | `user_texts`、`channel_names` |
| `doc_in/doc_qa` | 输入 | `doc_text`、`query_text`（TextBatch） | `raw_docs`、`raw_queries` |
| `keyword_in/keyword_match` | 输入 | `sentence_text`（TextBatch） | `input_sentences` |
| `rerank_in/cross_rerank` | 输入 | `query_text`（TextBatch）、`candidates`（RankedTextBatch） | `rerank_queries`、`rerank_candidates`；`rerank_pairs` 删除 |
| `entity_in/entity_extract` | 输入 | `sentence_text`（TextBatch） | `input_sentences` |
| `entity_in/translate` | 输入 | `query`（TextBatch） | `input_sentences` |
| `frame/ocr_invoice_qa` | 输入 | `image`（ImageRefBatch） | `image_paths` |
| `string/ocr_invoice_qa` | 输入 | `question`（TextBatch） | `user_queries` |
| `audio_out/audio_asr_intent` | 输出 | `transcribed_text`（TextBatch）、`intent_slot`（RuleMatchBatch） | `transcripts`、`intent_slots` |
| `audit_out/dialogue_audit` | 输出 | `verdict`（StructuredDocumentBatch）、`matched_policy`（RankedTextBatch，按请求聚合） | `structured_verdicts`、`matched_policy` |
| `doc_out/doc_qa` | 输出 | `answer_text`（TextBatch）、`intent`（RuleMatchBatch）、`chunk_count`（Int32Batch） | `llm_answers`、`intent_matches`、`doc_chunk_counts` |
| `keyword_out/keyword_match` | 输出 | `matches`（RuleMatchBatch） | `rule_matches` |
| `rerank_out/cross_rerank` | 输出 | `ranked`（RankedTextBatch） | `ranked_results` |
| `od_out/ocr_invoice_qa` | 输出 | `result`（StructuredDocumentBatch）、`document`（OcrDocumentBatch） | `extracted_invoice_json`、`ocr_docs` |
| `entity_out/entity_extract` | 输出 | `entities`（StructuredDocumentBatch） | `extracted_entities` |
| `entity_out/translate` | 输出 | `translation`（TextBatch） | `llm_answers` |

## 附录 C：等于默认值、迁移时删除的节点参数

统计范围为 `configs/` 下的方案，方案名省略前缀 `pipeline_` 和后缀 `.json`。迁移时全部删除。

| 节点 | 参数 | 值 | 方案 |
| --- | --- | --- | --- |
| `llm_generate` | `max_tokens` | `128` | dialogue_audit_kite、doc_qa_cpu、doc_qa_default、doc_qa_kite、doc_qa_kite_generated_embeddings、doc_qa_rerank_cpu、doc_qa_rerank_default、doc_qa_rerank_kite |
| `vector_top_k` | `min_score` | `0.0` | doc_qa_cpu、doc_qa_default、doc_qa_kite、doc_qa_kite_generated_embeddings、doc_qa_rerank_cpu、doc_qa_rerank_default、doc_qa_rerank_kite |
| `vector_top_k` | `top_k` | `1` | doc_qa_cpu、doc_qa_default、doc_qa_kite、doc_qa_kite_generated_embeddings |
| `text_rerank` | `top_k` | `1` | dialogue_audit_default、dialogue_audit_kite、doc_qa_rerank_cpu、doc_qa_rerank_default、doc_qa_rerank_kite |
| `text_template` | `separator` | `"\n"` | ocr_invoice_qa_kite |

下列参数随参数本身删除：`text_embedding.lifetime: "session"` 与 `vector_top_k.candidate_scope: "shared"`（dialogue_audit_default、dialogue_audit_kite 各一处）。

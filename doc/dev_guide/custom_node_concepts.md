# 写 Node 时的五个概念：它们分别替你解决什么问题

这篇说明配合[第一个自定义 Node](first_custom_node.md)阅读。你可以先完成一次练习，
遇到陌生名字时回来查；理解这些约定不要求先阅读整个 Core 或推理后端实现。

假设平台一次送进来两条文本：张三的材料、李四的材料。你希望分别整理输入、调用模型，
再把各自的结果交还平台。下面五个概念帮助你正确完成这件事。

## 1. 类型端口：这个操作接收什么、产生什么

把一个 Node 看作批量处理函数。输入端口是函数参数，输出端口是返回值；`TextBatch`
表示一组带来源编号的文本，而不是任意可以强制转换的内存。

在[模板源码](../../dev_support/node_authoring/starter_llm_node.cpp)中，`Input<TextBatch>("input")`
和 `Output<TextBatch>("output")` 声明逻辑名字和类型。Spec 同时生成运行时绑定与 Definition，
业务函数不直接读写 Blackboard。多输入使用 `InputsOf` 绑定普通输入结构，
多输出使用 `OutputsOf` / `Produced` 绑定普通结果结构；类型和逻辑端口只声明一次。
`Optional` 允许不连接端口；如果连接后当前请求仍可缺值，显式使用 `OptionalValue`，
由算法处理这个状态。`PortFlow` 声明非默认的数量、来源和生命周期属性。

有三个名字容易混淆：

| 名字 | 练习中的例子 | 谁决定它 |
| --- | --- | --- |
| 节点类型 `node_type` | `MyBusinessLlmNode` | C++ 注册；说明“这是什么操作” |
| 节点实例 `id` | `custom_prompt` | Pipeline；说明“这次使用这个操作的位置” |
| 逻辑端口 | `input`、`output` | Node；说明“操作接收、返回哪些值” |

同一个类型可以在一个方案出现多次，只需使用不同的 `id`。

Pipeline 还需要给传递的数据起名字，称为 Blackboard key。练习中的绑定关系是：

| 数据方向 | Node 中稳定的接口名 | 此方案的数据名 |
| --- | --- | --- |
| 读入 | `input` | `input_sentences` |
| 写出 | `output` | `llm_raw_answer` |

节点顶层的 `inputs` / `outputs` 负责这张映射。Validator 根据输入数据的唯一生产者
推导执行依赖；只有额外执行顺序需要填写可选的 `depends_on`。数组排列顺序不决定执行顺序。
必需输入必须显式连接，可选输入省略时表示未连接；输出未映射时仍使用逻辑端口名作为数据名。
换一个方案时，可以把 `input` 接到 `cleaned_texts`，不必改 C++ 中的接口名。

框架将值放在当前请求的 `AlgContext` 中，可以理解成“这次处理的数据工作区”。
Spec 包装读取只读输入，缺失或类型不符时记录错误；算法成功后发布返回值，同一个数据名不能重复写入。
需要修改文本时创建新值，不原地修改已经发布的输入，也不直接调用另一个 Node 绕过 Pipeline。

**第一次开发只需要做：** 选择正确的批类型、保持逻辑端口稳定，在 Pipeline 中连线。
已有 `TextBatch` 等类型不需要你编写类型注册；新增内部类型时再了解 `BlackboardTypeTraits`。
平台结构、指针和字符串容量属于 Adapter，不是节点端口的类型。

## 2. 来源编号：这条结果到底属于谁

`TextBatch` 中每一项是 `TraceableItem<std::string>`，包含三部分：

| 字段 | 含义 | 你通常怎样处理 |
| --- | --- | --- |
| `req_id` | 这项数据属于哪个请求 | 沿用输入的值，由平台接入链路提供 |
| `sub_id` | 同一请求内部的哪一项或哪一个片段 | 一对一处理时沿用；明确拆分时按节点契约生成 |
| `data` | 真正要处理的内容 | 文本、向量等业务算法的工作对象 |

例如输入是 `(101, 0, 张三的材料)` 和 `(202, 0, 李四的材料)`，输出中的张三结果仍应
标记 `(101, 0)`。循环下标 `0`、`1` 只代表本批中的位置，不能拿来替换请求编号。
一个请求拆成多个片段时，可能出现 `(101, 0)`、`(101, 1)`；它们仍属于同一个请求。

轻量模板声明 `1:1` / `preserve`：**输入一项，输出一项，顺序与两个编号都保持一致**。
在一般的批处理代码中，构造输出的写法是：

```cpp
outputs.emplace_back(item.req_id, item.sub_id, new_value);
```

模型可能在内部切批或补齐批次，框架的模型执行路径负责处理这些细节。节点仍要验证
返回数量与来源，因为“模型调用返回成功”不能证明每条回答都对应正确请求。

轻量模板把这项检查留在固定结构中。`BuildPrompt` 和 `FormatAnswer` 只接收文本，不接收
编号，因此常规业务修改可以集中在载荷上。需要过滤、拆分或聚合时，改用能表达该算法的
批处理实现，并同步声明数量和来源规则；不能让 1:1 模板悄悄丢掉一条输入。

## 3. 模型绑定：拿到一个已经准备好的模型能力

你需要的是“生成文本”的能力。`MakeLlmTextSpec` 组合一次模型调用；Batch 通过
`ModelsOf` 中的 `Model` 声明取得调用门面。成员类型决定能力：`LlmCall`、`EmbeddingCall`、
`AsrCall`、`OcrCall`、`RerankCall` 分别提供 `Generate`、`Embed`、`Transcribe`、`Recognize`、`Score`。
它们处理空批次、模型错误诊断及返回数量和来源检查，结果统一为 `NodeResult`。
模型失败直接传播，不为旧节点错误码再做一层映射。Node 不加载模型文件或创建厂商运行时。

下面几个字段承担不同职责：

| 字段 / 接口 | 练习中的值 | 说明 |
| --- | --- | --- |
| `model_type` | `test_biz_llm` | 哪一种模型语义实现；本例为测试模型 |
| `backend` | `test_causal_lm_backend` | 运行资源由哪一种后端实现提供；本例为测试后端 |
| `model_id` | `entity_llm` | Pipeline 为这个模型实例起的名字 |
| 节点配置 `bind_model` | `entity_llm` | 引用上面的模型实例，不是填写模型路径 |
| `ILlmModel` | C++ 接口 | 节点编译时依赖的能力约定 |

模型类型和后端名称只是这个练习的已注册配置，其他环境以 Catalog 为准。
模型能力由 `model_type` 对应的注册 Definition 提供，JSON 不再声明 `capability`。
`Model` 声明的引用字段必须显式填写，不能依靠约定模型名或候选模型数量自动选择。
模型和后端的组合需要通过协议校验；路径是否可加载、资源是否充足，还要在构建运行时确认。

Pipeline 构建期间准备模型资源，作者包装在初始化时取得各槽绑定的能力句柄。
资源属于会话，当前输入输出属于请求。本次提示词、回答、临时向量留在函数局部。
需要会话缓存时，`Run` 显式接收 `const SessionResources&`。普通工厂返回 `NodeResult<T>`，
交给 `GetOrCreateResult<T>(key, factory)`，得到 `NodeResult<std::shared_ptr<T>>`；框架复用资源、
向等待者保留完整失败信息，失败后允许重试。这是 Node 作者唯一的缓存创建入口。
缓存 key 必须包含影响结果的输入、参数和通过 `GetModelRevision` 取得的模型版本；
参考 [TextEmbeddingNode](../../src/common_nodes/text_embedding_node.cpp)。

换一个支持相同能力的模型时，通常更新 `models` 配置与 `bind_model` 即可。业务函数是否
仍适合新模型，要用实际数据确认。轻量模板使用 `GenerateOptions{}` 的默认采样参数；
需要调参时，使用带参数的 Spec / 自由 Batch，在普通函数中构造 options 并传给 `LlmCall`；
用 `Parameters` 的 `Field` 绑定结构成员与配置字段。

## 4. Definition：让连线工具和运行器看懂你的操作

编译后的 C++ 函数无法直接告诉 Studio“我需要文本输入，输出还是文本，并且要绑定一个
LLM”。`NodeDefinition` 就是把这些要求写成框架能读取的接口说明。

它主要回答：

| 声明 | 回答的问题 |
| --- | --- |
| `node_type`、`category`、`description` | 操作叫什么、归属哪里、用来做什么 |
| `inputs`、`outputs` | 哪些端口必需、类型是什么、数量与来源如何变化 |
| `config_fields` | 接受哪些参数、是否必填、默认值和范围是什么 |
| `model_dependencies` | 需要哪些模型能力槽位、哪个字段引用各模型实例 |
| `parallel_safe` | 节点自身是否满足并行调度的声明要求 |
| `biz_names` | 是否确有必要限制某些外部业务契约；通常留空便于复用 |

轻量模板只声明必填的 `bind_model`。因此复制完整样例的配置时，需要移除
`prompt_template`、`strip_markdown` 等它没有声明的字段。未知字段被拒绝，能尽早发现
“代码根本没有使用这个配置”的问题。

所有生产 Node 用 `REGISTER_FUNCTION_NODE` 从 Spec 生成构造方法和说明。Map、Batch、
LLM 便利组合使用同一契约；`NodeBase` 仅是框架内部运行机制。构建之后，Catalog、
Validator 和 Studio 自动使用注册结果，不需要你再维护 UI 节点列表。

**什么时候需要改 Definition？** 只改提示词构造或输出文本格式、接口保持不变时，通常
不用改。增加端口、参数、输出类型或改变数量关系时，必须一起更新声明和实现。

`Parameters<T>` 将字段声明、默认值和结构成员绑定在一处；语义校验用
`Validate` / `ValidateBindings`，由预检与初始化共用。参考
[自由 Batch starter](../../dev_support/node_authoring/starter_batch_node.cpp)。

Definition 会帮助原生校验发现类型、字段和连线错误，但不会自动实现业务代码。
Validator 根据 Definition 字段列表一次性校验未知字段、类型、范围和枚举，并填入默认值。
执行包装在初始化时消费 `ValidatedNodePlan` 的归一化配置与解析端口，缺少 Plan 时失败。
业务函数收到普通输入、参数和模型，返回 `NodeResult`；包装统一写入错误码与诊断。
跨字段及连线语义分别用 `Validate` / `ValidateBindings`，初始化与预检使用同一规则。
Control 更新单独归一化参数、构造下一状态后发布。

`Field` 成员必须显式声明 `.Required()` 或 `.Default(value)`，不能同时使用两者，也不从
结构体初值推断配置默认值。复杂数组/对象可用 `.WithParser(NodeConfigParser<YourConfig>(fields, parse))`
与基础绑定组合：合并字段并拒绝重名，复杂 parser 先产生持有自身数据的参数对象，随后赋基础成员，
再执行 `Prepare` 构建派生状态，最后执行 `Validate` 的跨字段规则及 `ValidateBindings` 的
连线规则。parser 接收已规范化 JSON，不要再次序列化；依赖基础参数的派生成员应在
`Prepare` 中重建。组合 `WithParser` 与字段 Control `WithControls` 时必须显式声明
`Prepare`，更新字段后框架会再次执行它，再校验和发布候选状态。

复杂参数的完整例子见
[复杂参数封装](../../src/custom_nodes/README.md#参数复杂时使用普通结构和解析封装)。

## 5. 并发声明：保证两个执行过程不会互相污染

假设两个调用同时使用相同的节点逻辑。函数局部的提示词和结果各自独立；如果把上次
输入放到成员 `last_input_`，另一次调用可能读取或覆盖它。即使暂时只串行执行，保存上次
请求也可能让后续请求携带旧数据。

所以 Node 的基本规则始终是：**不在节点成员里保存请求数据**。

| 可以作为成员 | 应当留在处理函数局部 |
| --- | --- |
| 初始化后固定或按 Control 约定安全更新的配置 | 当前输入文本、当前提示词 |
| 生命周期受管理的模型句柄 | 当前回答、临时处理结果 |
| 经设计和验证可安全共享的资源句柄 | 当前请求的 Context 指针、临时请求缓存 |

“无请求状态”允许节点持有配置。在线更新时先校验新值，失败保留旧值，每次处理读取
一致快照。普通字段使用 `WithControls`；复杂命令使用 `WithControl` 声明 schema 和
构造下一状态的函数，框架串行处理更新并发布。见 [Control 练习](first_control.md)。
复杂状态的初始构建与更新复用普通 `Build...State` 函数；更新回调负责构建完整候选，
`WithControl` 不会再次运行初始化的 `Prepare`。模板编译和规则解析各有一个实现，
配置与补丁可以使用不同结构。

`parallel_safe=false` 是脚手架的保守初始声明。Pipeline 的 `max_parallel_workers` 大于 1 时，若该节点
处于含多个节点的并行层，Validator 会拒绝这个计划；它不会自动加锁，也不会自动把
那一层改成串行。`max_parallel_workers` 范围为 1–64，默认 1；本练习沿用默认串行执行。

准备设为 `true` 时，检查你的业务函数、调用的辅助对象和所有节点自有共享状态。
`true` 只说明节点自身可以按该契约并行执行；所绑定模型及 Backend 的并发约束仍由
框架独立检查，并不是这个布尔值能解除的限制。

## 复杂算法仍按普通 C++ 函数组织

多个输入、条件二次推理或复杂后处理都组织在普通 `Run` 函数中。保序输出用
`PreservedOutput` 声明 anchor，框架检查等长、同序和同来源；局部选择后需要回填完整批次。
拆分、排名或源输出用 `ProducedBatch` 和真实 `PortFlow`；多输出用 `OutputsOf` / `Produced`。
每个输出端口必须绑定独立的结果成员；重复绑定同一成员会在声明时被拒绝，避免发布时重复移动。
框架在发布前检查全部带 anchor 的输出，派生输出的编号和数量正确性由算法及测试保证。

12 个生产 Node 都使用这套 Spec，包括 OCR 双输出、TextChunk 拆分、TextCorpusSource 源输出、
TextEmbedding 会话缓存和两种复杂 Control。无需按场景维护另一套生命周期写法。
函数较多时可拆成操作相关的 `.h/.cpp`，登记同目录 CMake，保持目录按操作组织。

从下面的现有实现中只取需要的部分：

| 具体问题 | 可复用代码与边界 |
| --- | --- |
| 输入与模型输出一一对应 | [ValidatePreservedTraceableAlignment](../../include/nodes/traceable_batch_validation.h) 检查数量、顺序和两个来源编号；真实过滤/聚合不能套用 1:1 校验 |
| 两批数据按完整来源关联 | [Join 示例](../../dev_support/node_authoring/starter_batch_join_node.cpp) 使用 `JoinByItem`；显式选择 exact/left，右侧未知 key 均失败 |
| 按请求收集参考内容并保留空组 | [Group 示例](../../dev_support/node_authoring/starter_batch_group_node.cpp) 使用 `GroupByRequest`；按原 anchor 位置查询组，保持 A0/B0/A1 原序 |
| 只对部分结果再次推理 | [Select/Scatter 示例](../../dev_support/node_authoring/starter_batch_select_scatter_node.cpp) 先 `SelectBatch`、显式 `Materialize()`、调用模型，再 `ScatterReplace`；无选中项时跳过第二次调用 |
| 拆分载荷并分配子编号 | [TextChunkNode](../../src/common_nodes/text_chunk_node.cpp) 使用 `SplitPayloads`；每个请求连续分配子编号，counts 保留父 key，载荷回调只负责切分 |
| 多个问题各自配多段材料 | [PromptGuidedLlmNode](../../src/custom_nodes/prompt_guided_llm_node.cpp) 按 `req_id` 收集 context，主输出沿用 input 的 `(req_id, sub_id)` |
| 候选打分、按请求分组、保留原候选来源 | [TextRerankNode](../../src/common_nodes/text_rerank_node.cpp) 展示来源检查后再排序；新 rank 与原候选编号分别保存 |
| 字段、默认值与范围 | [ValidateAndNormalizeFields](../../include/contracts/config_schema_validation.h)，Validator 消费 Definition 字段列表，Init 读取 Plan 中的归一化结果 |
| 多字段配置转为普通参数结构 | [NodeConfigParser](../../include/nodes/node_config_parser.h)，复用字段校验与节点自己的语义解析 |
| 初值与运行时更新使用同一业务校验 | [TextTemplateNode](../../src/common_nodes/text_template_node.cpp) 使用 `WithControl`，失败不替换旧配置 |
| 提示词变量替换 | [现有模板工具](../../include/nodes/text_template.h)，只在实际需要模板语义时使用 |

批次工具由 `nodes/authoring.h` 提供，返回 `NodeResult`。Join/Group/Selection 借用输入，
拒绝临时批次；使用期间输入必须存活且不修改、不移动。视图仅用于本次请求内的同步算法，
不能保存到 Node/Session 或异步任务。`Materialize`、Scatter 和 Split 的输出拥有数据。
错误在 AuthorNode 边界统一写入诊断，保留回调错误码、内容及完整来源 key；普通算法不提前
写 Context。工具要求完整 key 唯一，不改变未使用这些工具的 Map/模型重复 key 行为。

先声明结果数量和来源，再编码。例如“两条输入各输出一条”必须保留两组编号；“每个问题
取前三个候选”要按请求分组并声明排名来源，不能用整个 batch 的前三项代替。
多输入不能仅凭数组下标配对：一对一数据用两个编号关联，片段聚合按声明的请求关系处理。
空批次、某个请求没有候选、模型少返回一项，都应在算法测试中有明确预期。

把输出构造在局部变量中，全部成功后返回 `NodeResult::Success`；失败返回
`NodeResult::Failure`，原因写明字段或来源。执行包装统一处理诊断和发布，普通算法不保存
Context 指针。配置更新由包装提供一致快照；含外部资源的更新仍需明确资源生命周期。

第一次测试可复用 [InitNodeForTest](../../tests/support/node_test_utils.h)，将断言加入现有
Node 套件；命令见[局部测试路径](../../tests/README.md#fast-feedback-for-solution-authors)。
先用两个不同请求、非零 `sub_id` 验证不会串结果，再检查算法自己的正常和失败输出。
无需为了组织一个复杂 Node 再增加继承层、配置语言或专用测试执行器。

## 从一次运行看这些概念如何配合

1. **构建时**：注册提供 Definition；Validator 检查连线、配置和模型引用，产生执行计划。
2. **初始化时**：Spec 包装按计划绑定端口、解析参数、取得声明的模型能力。
3. **处理时**：包装读取输入和一致参数；业务函数处理 `.data`，返回结果；检查保序输出后发布。
4. **返回时**：Adapter 读取方案的最终输出，按平台契约完成转换与拷贝。

常见问题可按下面的顺序定位：

| 现象 | 先检查什么 |
| --- | --- |
| Catalog 找不到新节点 | 文件是否登记进 CMake、是否重新构建、执行的是否是刚构建的工具 |
| 未知参数或缺失 `bind_model` | 当前 Definition、节点 config、`models[].model_id` |
| 输入类型或生产者不匹配 | `inputs` / `outputs` 两端的类型、实际数据名和唯一生产者 |
| 模型调用返回错误 | 节点报告的错误码、所绑定模型的日志和资产配置 |
| 输出数量或来源不匹配 | 前后处理是否删项/换序/改编号，模型是否正确保留来源 |
| 并行计划被拒绝 | 节点声明以及同层所使用模型的并发能力 |

需要核对精确接口时，再查阅 [Node 作者接口](../../include/nodes/authoring.h)、
[模型调用门面](../../include/nodes/model_calls.h)、
[模型能力接口](../../include/engine/model_interface.h)和
[Definition 声明](../../include/core/pipeline_catalog.h)。

# 写 Node 时的五个概念：它们分别替你解决什么问题

这篇说明配合[第一个自定义 Node](first_custom_node.md)阅读。你可以先完成一次练习，
遇到陌生名字时回来查；理解这些约定不要求先阅读整个 Core 或推理后端实现。

假设平台一次送进来两条文本：张三的材料、李四的材料。你希望分别整理输入、调用模型，
再把各自的结果交还平台。下面五个概念帮助你正确完成这件事。

## 1. 类型端口：这个操作接收什么、产生什么

把一个 Node 看作批量处理函数。输入端口是函数参数，输出端口是返回值；`TextBatch`
表示一组带来源编号的文本，而不是任意可以强制转换的内存。

在[基础源码](../../dev_support/node_authoring/starter_llm_node.cpp)中，`Input<TextBatch>("input")`
和 `Output<TextBatch>("output")` 声明逻辑名字和类型。Spec 同时生成运行时绑定与 Definition，
业务函数不直接读写 Blackboard。高级生命周期路径仍可使用 `BoundInput` / `BoundOutput`。

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

`ports.inputs` / `ports.outputs` 负责这张映射，`depends_on` 明确节点执行依赖。
换一个方案时，可以把 `input` 接到 `cleaned_texts`，不必改 C++ 中的接口名。

这些值放在当前请求的 `AlgContext` 中，可以理解成“这次处理的数据工作区”。`Require`
读到只读输入；缺失或类型不符时会记录错误。`Set` 发布输出，同一个数据名不能重复写入。
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

你需要的是“生成文本”的能力。基础模板的 `MakeLlmTextSpec` 负责一次模型调用；自由 Batch
通过 `ModelsOf` 中的 `Llm` 槽取得 `LlmCall`，调用 `Generate` 得到可检查的 `NodeResult`。
高级路径仍可使用 `ModelBoundNode<ILlmModel>`。节点不用自己加载模型文件、创建厂商运行时
或按 Backend 名称写分支。

直接调用五种 `IModel` 能力时，最后一个可选参数是 `std::string* diagnostic`。调用方用
局部字符串接收失败原因，并连同返回码传给 `Fail`；`LlmCall`、`EmbeddingCall` 及生成的
高级模板已经处理该传递。模型实现应在入口清空诊断，失败时保留 Backend 原因，成功或
空批次不留下旧错误；不要把“上一次错误”保存在共享模型成员中。

下面几个字段承担不同职责：

| 字段 / 接口 | 练习中的值 | 说明 |
| --- | --- | --- |
| `capability` | `llm` | 模型提供哪一类能力 |
| `model_type` | `test_biz_llm` | 哪一种模型语义实现；本例为测试模型 |
| `backend` | `test_causal_lm_backend` | 运行资源由哪一种后端实现提供；本例为测试后端 |
| `model_id` | `entity_llm` | Pipeline 为这个模型实例起的名字 |
| 节点配置 `bind_model` | `entity_llm` | 引用上面的模型实例，不是填写模型路径 |
| `ILlmModel` | C++ 接口 | 节点编译时依赖的能力约定 |

模型类型和后端名称只是这个练习的已注册配置，其他环境以 Catalog 为准。
模型和后端的组合需要通过协议校验；路径是否可加载、资源是否充足，还要在构建运行时确认。

Pipeline 构建期间准备模型资源，作者包装在初始化时取得各槽绑定的能力句柄。
资源属于会话 `SessionContext`，当前输入输出属于请求 `AlgContext`。模型句柄可以是成员，
本次提示词、回答、临时向量应留在处理函数局部。

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

基础接口用 `REGISTER_FUNCTION_NODE` 从 Spec 生成构造方法和说明；高级接口继续用
`REGISTER_NODE_WITH_DEFINITION` 显式注册。构建之后，Catalog、
Validator 和 Studio 自动使用注册结果，不需要你再维护 UI 节点列表。

**什么时候需要改 Definition？** 只改提示词构造或输出文本格式、接口保持不变时，通常
不用改。增加端口、参数、输出类型或改变数量关系时，必须一起更新声明和实现。

基础接口的 `Parameters<T>` 将字段声明、默认值和结构成员绑定在一处；语义校验用
`Validate` / `ValidateBindings`，由预检与初始化共用。参考
[自由 Batch starter](../../dev_support/node_authoring/starter_batch_node.cpp)。

Definition 会帮助原生校验发现类型、字段和连线错误，但不会自动实现业务代码。
Validator 根据 Definition 字段列表一次性校验未知字段、类型、范围和枚举，并填入默认值。
Init 必须收到 `ValidatedNodePlan`，读取其中 `normalized_config` 和已解析端口；
缺少 Plan 时失败。跨字段约束提取成局部函数，由 Init 与 `validate_config` 共用，
初始化仍负责资源准备和语义检查。Control 更新单独归一化新参数后再发布。
节点执行失败使用 `Fail/Require`，让返回码与请求诊断一致；初始化用 `init_ctx.Fail(reason)`
传递具体原因。
具体写法可按需参考 `PromptGuidedLlmNode`，第一天不必复制它的全部参数和解析逻辑。

基础成员必须显式声明 `.Required()` 或 `.Default(value)`，不能同时使用两者，也不从
结构体初值推断配置默认值。复杂数组/对象可用 `.WithParser(NodeConfigParser<YourConfig>(fields, parse))`
与基础绑定组合：合并字段并拒绝重名，复杂 parser 先产生持有自身数据的参数对象，随后赋基础成员，
最后执行 `Validate` 的跨字段规则及 `ValidateBindings` 的连线规则。parser 接收已规范化 JSON，
不要再次序列化；复杂派生成员若依赖基础参数，使用最终语义校验检查它们。

高级接口参数较多时可使用 `NodeConfigParser<YourConfig>`，将字段声明和语义解析放在一起，
得到节点自己的普通参数结构。它复用上述通用校验并直接传递 JSON 对象；不增加
序列化步骤，也不要求修改 Node 基类或 Pipeline。入口选择及完整例子见
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
一致快照；具体同步与更新写法见 [Control 练习](first_control.md#3-阅读四个编辑点)。

`parallel_safe=false` 是脚手架的保守初始声明。对于显式 `parallel` Pipeline，若该节点
处于含多个节点的并行层，Validator 会拒绝这个计划；它不会自动加锁，也不会自动把
那一层改成串行。本练习沿用默认的 sequential 执行模式。

准备设为 `true` 时，检查你的业务函数、调用的辅助对象和所有节点自有共享状态。
`true` 只说明节点自身可以按该契约并行执行；所绑定模型及 Backend 的并发约束仍由
框架独立检查，并不是这个布尔值能解除的限制。

## 复杂算法仍按普通 C++ 函数组织

多个输入、条件二次推理或复杂后处理可组织在普通自由 Batch 函数中：显式声明 anchor，
最终返回与它等长、同序、同来源的 `PreservedOutput`。局部选择后须回填完整批次。
需要发布拆分后的子项和父项计数等不同来源/数量的端口时，继续使用 `NodeBase`；需要模型句柄时用
`ModelBoundNode`。`ProcessNode` 负责读端口、取得本次配置快照、调用你的算法函数、检查
结果并发布。算法函数使用普通值、容器和局部变量；函数较多时拆成操作相关的 `.h/.cpp`，
再登记同目录 CMake，保持 `custom_nodes` 按操作组织。复杂程度本身不要求修改 Core。

从下面的现有实现中只取需要的部分：

| 具体问题 | 可复用代码与边界 |
| --- | --- |
| 输入与模型输出一一对应 | [ValidatePreservedTraceableAlignment](../../include/nodes/traceable_batch_validation.h) 检查数量、顺序和两个来源编号；真实过滤/聚合不能套用 1:1 校验 |
| 两批数据按完整来源关联 | [Join 示例](../../dev_support/node_authoring/starter_batch_join_node.cpp) 使用 `JoinByItem`；显式选择 exact/left，右侧未知 key 均失败 |
| 按请求收集参考内容并保留空组 | [Group 示例](../../dev_support/node_authoring/starter_batch_group_node.cpp) 使用 `GroupByRequest`；按原 anchor 位置查询组，保持 A0/B0/A1 原序 |
| 只对部分结果再次推理 | [Select/Scatter 示例](../../dev_support/node_authoring/starter_batch_select_scatter_node.cpp) 先 `SelectBatch`、显式 `Materialize()`、调用模型，再 `ScatterReplace`；无选中项时跳过第二次调用 |
| 拆分载荷并分配子编号 | [TextChunkNode](../../src/common_nodes/text_chunk_node.cpp) 使用 `SplitPayloads`；每个请求连续分配子编号，counts 保留父 key，载荷回调只负责切分 |
| 多个问题各自配多段材料 | [PromptGuidedLlmNode::ProcessNode](../../src/custom_nodes/prompt_guided_llm_node.cpp) 按 `req_id` 收集 context，主输出沿用 input 的 `(req_id, sub_id)` |
| 候选打分、按请求分组、保留原候选来源 | [TextRerankNode::ProcessNode](../../src/common_nodes/text_rerank_node.cpp) 展示来源检查后再排序；新 rank 与原候选编号分别保存 |
| 字段、默认值与范围 | [ValidateAndNormalizeFields](../../include/contracts/config_schema_validation.h)，Validator 消费 Definition 字段列表，Init 读取 Plan 中的归一化结果 |
| 多字段配置转为普通参数结构 | [NodeConfigParser](../../include/nodes/node_config_parser.h)，复用字段校验与节点自己的语义解析 |
| 初值与运行时更新使用同一业务校验 | [Control 模板](../../dev_support/node_authoring/starter_control_node.cpp) 的局部解析函数，失败不替换旧配置 |
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

把本次输出构造在局部变量中，全部成功后再 `Set`。失败通过 `Fail(req_ctx, code, reason)`
返回，原因写明涉及的字段或来源；初始化失败使用 `init_ctx.Fail(reason)`，Pipeline 会补上
实例 ID 和类型。`diagnostic` 只在 Init 调用期间有效，不保存它的指针。
配置需要在线修改时，为一次请求取一致快照；含外部资源的更新应先设计资源生命周期。

第一次测试可复用 [InitNodeForTest](../../tests/support/node_test_utils.h)，将断言加入现有
Node 套件；命令见[局部测试路径](../../tests/README.md#fast-feedback-for-solution-authors)。
先用两个不同请求、非零 `sub_id` 验证不会串结果，再检查算法自己的正常和失败输出。
无需为了组织一个复杂 Node 再增加继承层、配置语言或专用测试执行器。

## 从一次运行看这些概念如何配合

1. **构建时**：注册提供 Definition；Validator 检查连线、配置和模型引用，产生执行计划。
2. **初始化时**：基类取得模型句柄；`BindPort` 根据计划把逻辑端口接到实际数据名。
3. **处理时**：`Require` 读取本次输入；业务函数处理 `.data`；模型生成后检查来源，再发布结果。
4. **返回时**：Adapter 读取方案的最终输出，按平台契约完成转换与拷贝。

常见问题可按下面的顺序定位：

| 现象 | 先检查什么 |
| --- | --- |
| Catalog 找不到新节点 | 文件是否登记进 CMake、是否重新构建、执行的是否是刚构建的工具 |
| 未知参数或缺失 `bind_model` | 当前 Definition、节点 config、`models[].model_id` |
| 输入类型或生产者不匹配 | `ports` 两端的类型、实际数据名和 `depends_on` |
| 模型调用返回错误 | 节点报告的错误码、所绑定模型的日志和资产配置 |
| 输出数量或来源不匹配 | 前后处理是否删项/换序/改编号，模型是否正确保留来源 |
| 并行计划被拒绝 | 节点声明以及同层所使用模型的并发能力 |

需要核对精确接口时，再查阅 [Node 支持代码](../../include/nodes/node_base.h)、
[模型绑定基类](../../include/nodes/model_bound_node.h)、
[模型能力接口](../../include/engine/model_interface.h)和
[Definition 声明](../../include/core/pipeline_catalog.h)。

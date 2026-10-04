# 架构审查：结论与优化建议

| 项 | 内容 |
| --- | --- |
| 状态 | 第六节事项已决定；改进项按[实施进度](#七实施进度)逐项落地 |
| 基线 | `main@a664e37`（2026-10-03） |
| 范围 | 审查四层源码、构建与分层守卫、测试和工具；本次仅修订审查文档，未修改代码 |
| 约束 | 保持 Operator 函数表、宿主结构体、槽位名、错误码枚举集合、`.conf` 和 Pipeline JSON 格式；错误码返回语义的调整须单独决策，不能视为内部重构 |

本文件按 [plans/ 的规则](README.md)存放：它是工作文档，不是现行规则。各项落地后，把形成的规则写进对应的现行指南；
全部完成或转为 RFC 后删除本文件。文中行号以基线为准。

**审查标准**（来自需求方）：

1. 对外契约不变：这是平台调用本框架生成的库的固定方式。
2. 每一层新增内容都方便，例如新增统一接口的 Node、新增 Backend。
3. 抽象层级之间相对隔离。
4. 框架处理大部分调度，使用者具备初步 C++ 和少量框架知识即可开发业务。
5. 框架本身容易理解：没有不必要的概念，没有可以合并或抽象却散落各处的代码。

**验证方式**：标"已复现"的两项用现有 `build/` 产物实际运行，步骤见[附录](#附录复现步骤)。
复核还执行了 10 个相关测试，全部通过，筛选命令见[附录 C](#c-复核测试)。其余判断来自基线代码、现行指南和测试断言。
这些证据不代表真实厂商运行时或目标硬件验收；`include/platform_mock/` 中的声明只是仓库约定，
真实 SDK 的契约只能在授权内网中核验。

## 结论

整体架构合理，不需要推倒重来。主线是：Operator 门面 → 校验后不可变的执行计划 → 带类型端口的节点 DAG →
模型能力接口 → 后端执行协议。每一段职责清楚，异常拦截、输出池回滚、厂商头文件隔离都做得扎实。
应采纳核心问题判断，按采纳条件逐项实施；本文不是已批准的整体重构方案。

| 分级 | 项目 | 处理原则 |
| --- | --- | --- |
| 优先修复 | 端口数量关系误判；通用 Node 承担外部响应序列化 | 修复合法流程与职责边界，保留来源检查和现有响应格式 |
| 优先决策 | 错误码的公开语义；致命错误与单条业务无效结果的区别 | 明确行为与验收后再实施，不能按内部重构处理 |
| 后续重构 | Map/Batch 生命周期、端口绑定、配置声明与实例状态等重复 | 逐项证明收益，保持诊断、所有权和并发契约 |
| 保留或暂缓 | 转换器契约元数据、自定义 allocator、原子快照、Model 来源接口 | 不因当前只有测试使用或外层已有锁就删除 |

新增 Node 已经很顺手；Model、Backend 和外部业务接入仍有简化空间，但扩展步骤多并不等于其中的契约检查冗余。

**建议保留的设计**：

- 门面的 `noexcept` 与双重 catch、先解码再租用输出、租约自动回滚、输出 deleter 只持有弱引用。
- 校验一次生成不可变计划，Pipeline 只消费不重算；模型批量原子注册；构建状态机。
- "普通函数 + Spec"的节点写法和 `MakeLlmTextSpec`；签名错误有编译期提示；业务目录源码自动收录。
- 参数快照的原子发布、失败回滚、旧快照存活和整批配置一致性。
- Model 与 Backend 分离，经中性执行协议对接；厂商头文件只出现在 Backend 的源文件里。
- Catalog 作为唯一事实来源，Studio 经 CLI 调用 Validator，不复制规则。

## 一、优先处理的问题与契约决策

### 1. 内部错误码原样返回给宿主（已复现）

仓库的 mock 错误码定义了 0、-1…-7、-99、-100（[error_codes.h](../include/platform_mock/error_codes.h)），
`Process` 还会把 Pipeline 的返回值原样交给宿主（[operator_adapter.cpp:388](../src/adapter/operator/operator_adapter.cpp#L388)）。
这存在数值碰撞和故障分类不清的问题，但不能据此断言真实平台只有上述错误码。

- **实测**：一个 `StructuredJsonParseNode(failure_policy=fail)` 的方案，`ops.Process` 返回 **-6102**，不在枚举里（附录 A）。
- **会撞上已有错误码**：
  - Qwen 推理失败返回 -1（[qwen_causal_lm_model.cpp:146-149](../src/engine/models/qwen_causal_lm/qwen_causal_lm_model.cpp#L146-L149)），
    经 [model_calls.h:52-59](../include/nodes/model_calls.h#L52-L59) 和 [function_node.h:1637](../include/nodes/function_node.h#L1637)
    原样上传；宿主若只按 mock 枚举解释，会误判为"无效句柄"。
  - 批处理执行器捕获异常返回 -4（[fixed_batch_executor.h:122](../include/engine/fixed_batch_executor.h#L122)），
    与门面表示"输出容量不足"的 -4 相同。
  - Create 时配置校验失败、模型加载失败都返回 -3"输入非法"
    （[io_binding_resolver.cpp:221](../src/adapter/io_binding_resolver.cpp#L221)、
    [shared_algorithm_runtime.cpp:131](../src/adapter/shared_algorithm_runtime.cpp#L131)）。
- **现行契约**：[宿主调用与生命周期](../doc/dev_guide/operator_output_allocation.md#宿主调用与生命周期)明确允许透传，
  并要求结合诊断判断故障层；[契约测试](../tests/contract/abi/test_adapter_contract_security.cpp#L262)还断言
  模型错误 -731 原样返回。因此，映射属于公开返回行为的调整，不能称为"不涉及外部契约"的局部修复。
- **采纳方向**：在接入适配层集中定义公开错误映射，按失败阶段或类别分类，原始码、节点及所在层级保留在
  `GetOperatorLastError()`。不能只按整数映射：模型执行的 -4 与输出容量的 -4 含义不同。
  现有 mock 枚举没有单独的普通执行失败类别；在保持枚举集合的约束下，目标码及其解释须先决定（见第六节）。
- **验收**：同步现行指南与错误码契约测试，覆盖 Create、输入转换、节点/模型执行、输出容量、Control 和异常路径；
  核实宿主看到的分类不会因内部错误码碰撞而变化。真实 SDK 对接仍需另行核验，不能承诺仅改一个映射函数即可完成。

### 2. 端口数量关系校验会拒绝合法流程（已复现）

[pipeline_validator.cpp:104-110](../src/core/pipeline_validator.cpp#L104-L110) 要求生产端和消费端的 cardinality
一致（除非一方是 `N:M`，或消费端是 `N:1`）。现有声明混用了节点自身的数量转换关系与边上数据的数量约束，
按字符串直接比较会拒绝可合法执行的组合：

- **拆分后接不了逐项节点**：`TextChunkNode → TextRuleMatchNode` 报 `PORT_CARDINALITY_MISMATCH`（附录 B）。
  RuleMatch 逐条处理并保留来源编号 (req_id, sub_id)，这个流程本来是正确的。
- **自定义 Map 节点同样受限**：`MakeMapSpec` 把输入写死为 `1:1`（[function_node.h:309](../include/nodes/function_node.h#L309)），
  不能放在任何拆分节点之后。按同一逻辑，聚合输出（`N:1`）也接不了逐项节点。
- **Embedding 能接在拆分之后，是因为它声明了 `N:M`**，跳过了这项 cardinality 比较；类型、来源和存活期检查仍在。
- **来源和存活期需分别评价**：当前生产消费者多声明 `preserve` 或 `aggregate`，静态 provenance 比较通常放行；
  这不代表运行时来源校验无用。`global` 和 `independent` 暂无生产声明，`session` 则用于语料和 Embedding 缓存，
  不能把整套检查一起删除。

**采纳方向**：区分节点的转换关系与边上的数据形状。逐项节点接收任意条目批次，并保留输入的数量、顺序及
`(req_id, sub_id)`；拆分和聚合显式声明输出关系；出口仍须满足业务要求。不能仅把旧字符串替换为
"每请求一条、每请求多条、句柄共享"三种标签：数量、来源关系和存活期是不同维度。

**设计与验收**：补齐空结果、可选输入、多输入对齐、共享候选、拆分后的逐项变换和聚合后再变换的规则；
保留真正不兼容的出口和来源错误，不能用普遍声明 `N:M` 或关闭检查来绕过。Pipeline JSON 格式保持不变，
Definition、Catalog、Validator、节点运行时及工具的解释须一致。完整形状推导属于跨层设计，不能预先归为局部修复。
[框架简化计划](FRAMEWORK_SIMPLIFICATION_PLAN.md)第 8 节的阶段 4（跨请求状态）准备在这套字符串上再加 `shared` 和
`provenance_config_field`，应先明确这里的数量与来源语义，再推进状态工作；无需等待第三节所有重构完成。

### 3. 外部响应格式由通用节点生成

`TextRuleMatchNode` 自己拼 `match_result_json`（[text_rule_match_node.cpp:318-329](../src/common_nodes/text_rule_match_node.cpp#L318-L329)），
两个输出转换器原样写出（[keyword_result_output.cpp:22-25](../src/adapter/output/keyword_result_output.cpp#L22-L25)、
[audio_result_output.cpp:71-72](../src/adapter/output/audio_result_output.cpp#L71-L72)）。改节点里一个键名，
就同时改了两个外部契约，违反了 [AGENTS.md](../AGENTS.md) 中"响应组装和序列化留在 Adapter"的规则。

`RuleMatchItem`（[common_contracts.h:64](../include/core/common_contracts.h#L64)）的标量字段、`details` 和序列化字符串
有信息重叠，但并非三份完全相同的结果：标量字段主要保存首次命中，`details` / JSON 还包含全部 `matches`、
带 JSON 类型的 slots 常量，以及默认命中的 `raw_query`。只用现有标量字段重建响应会丢失信息。

**采纳方向**：Node 输出完整中性匹配结果，由 Adapter 按各自外部契约组装和序列化。内部业务状态与宿主
`status_code` 的映射也归 Adapter，不要求删除所有内部状态表达。

**验收**：覆盖关键词与音频两种外部响应，保持全部字段、类型、默认值和序列化结果；同步检查读取 `details` 的
内部消费者，覆盖多规则命中、typed slots 和默认命中，不能在搬移序列化时悄悄缩减语义。

### 4. 致命错误与单条业务无效结果的区别（待明确）

任一 Node 返回非零失败，或转换器拒绝结果时，整批不发布新输出；审计结果的 `risk_level` 不在枚举内也会触发
转换失败。整批回滚和租约归还有[契约测试](../tests/contract/abi/test_operator_safety.cpp#L261)。
这是当前的致命错误语义，应与单条业务结果无效分开描述。

内置成功路径通常产生 `status_code=0`，但框架没有强制它恒为 0：关键词、音频、文档转换器会复制内部状态，
[行编码测试](../tests/unit/adapter/test_io_converters.cpp#L697)明确验证非零状态 23 被保留。
`StructuredJsonParseNode` 也已有逐项 `is_valid`、`parse_status` 和 `diagnostic`；
`emit_diagnostic` / `configured_fallback` 可使坏项不触发节点失败，见
[测试](../tests/unit/nodes/test_structured_json_parse_node.cpp#L94)。下游是否接受这些结果仍由具体契约决定。

**建议**：保持致命错误的整批原子性，在各业务契约中明确单条无效结果、fallback 和 `status_code` 的含义。
不能把保留整批回滚等同于"所有 status_code 恒为 0"，也不能仅因某业务需要单条失败就认定必须新增框架级错误通道。
若确有通用部分成功需求，再设计失败项的来源、下游过滤/聚合、输出组装及跨请求状态提交规则，并单独验收。

## 二、对照五条标准

| 标准 | 现状 | 主要差距 |
| --- | --- | --- |
| 1. 外部契约不变 | ABI 与载体保持稳定；现行返回行为包含内部码透传 | 错误分类易碰撞；调整返回语义需要契约决策（问题 1） |
| 2. 各层新增方便 | 新增 Node 很顺；Model、Backend、业务契约偏重 | 见第四节 |
| 3. 层次隔离 | 编译期隔离很强 | 响应格式在节点；业务边界与提示文案在 Core 可进一步分离；中性来源编号本身不构成越层 |
| 4. 使用者门槛 | 编排和简单节点低；Batch 节点中等；新业务契约高 | 端口三元字符串、`TraceableItem`、Adapter 标识过多 |
| 5. 易理解、无冗余 | 存在重复实现和未使用结构，也有必要的契约与扩展接口 | 见第三节；不能把仅由测试使用的能力一律视为死代码 |

## 三、简化建议及采纳条件

本节区分实现重复、未使用结构与已支持的扩展能力。仅由测试使用不是删除理由；撤销能力、改变并发保证或
改变配置含义都需要按契约变更评价，不能统一归为低风险内部清理。

### 接入适配层

- **部署信息有多处转存，可减少重复字段**：
  [PreparedDeployment](../src/adapter/deployment_preparation.h#L25) →
  [ValidatedIoPlan](../src/adapter/io_binding_resolver.h#L19) →
  [ResolvedOperatorConfig](../src/adapter/operator/operator_config_resolver.h#L18) →
  [SharedAlgorithmRuntime](../src/adapter/shared_algorithm_runtime.h) 和
  [OperatorHandle](../src/adapter/operator/operator_adapter.cpp#L27)。这些对象分别承担部署准备、校验结果、宿主配置和
  可变运行资源，不能仅按对象数量判断冗余。`SharedAlgorithmRuntime` 还承担注册审计、Pipeline 构建和异常处理，
  并非只做转发（[shared_algorithm_runtime.cpp:24](../src/adapter/shared_algorithm_runtime.cpp#L24)）。
  可研究共享不可变部署数据及更准确的命名，但须保留预检与资源加载的阶段边界、失败原子性和输出池生命周期。
- **端口绑定实现重复，可抽公共实现**：`InputPortBindings` 和 `OutputPortBindings`
  （[io_converter.h:150](../include/adapter/io_converter.h#L150)、[io_converter.h:194](../include/adapter/io_converter.h#L194)）
  的查找与存储逻辑相同。保留输入/输出方向约束及错误诊断即可，不必扩展作者接口。
- **输入限额检查（B3 已处理）**：值类型校验与转换器各查一次限额，但两处都取自 `biz_input` 常量，
  分别负责读取宿主内存前的安全上限和业务字段语义，保留两层检查。原 `ResolvedInputLimits` 从未被配置，
  已改名为 `InputLimits` 并移出部署配置与 handle；通用载体的 10 MiB 上限也改为具名常量，
  读取范围和错误优先级不变。
- **契约元数据应保留，可减少重复填写**：`schema_id`、`schema_version`、`external_type`、槽位 `value_type` 和
  `capacity_policy` 实际参与同一业务外部协议的一致性检查，不一致会拒绝接入，见
  [io_binding_registry.cpp:14](../src/adapter/io_binding_registry.cpp#L14)。其中部分已有默认值，并非每项都要手写。
  可由工厂或类型推导重复的载体信息，但须保留协议身份、版本和容量策略检查，不能把这些字段视为无效元数据。
- **自定义输出分配器应保留**：当前生产类型没有额外具名 allocator，但注册、参数解析与 `OutputConfigReader`
  是已支持的扩展能力（[operator_value_type.h:310](../include/adapter/operator_value_type.h#L310)），用于嵌套布局、
  数组容量及分配/释放。`allocator`、`params` 也是现有 Pipeline 配置字段，见
  [输出分配方案](../doc/dev_guide/operator_output_allocation.md#选择参数)。测试正在证明这条接入路径。
  删除属于撤销能力，并非死代码清理；本文不建议删除。
- **未使用结果结构已清理（B1）**：原 `include/adapter/biz_results.h` 中 6 个结构没有类型引用，已随文件删除；
  `KeywordResult` 只用于输出池与自定义 allocator 测试，已改为测试夹具中的 `NestedOutputSource`。
- **宿主类型集中登记可研究分散注册**：类型清单写在框架头里（[io_converter.h:35](../include/adapter/io_converter.h#L35)），
  内置值类型集中在 [operator_builtin_value_types.cpp](../src/adapter/operator/operator_builtin_value_types.cpp)，
  新增宿主结构要改这两个集中文件。收敛注册步骤须保留类型与布局核验，不把宿主类型带入下层；
  外部环境只能处理现有 mock 和中性接缝，不能推测真实 SDK 的新类型。

### 流程编排层

- **业务审计与图校验可进一步分离**：Validator 同时使用 `BizDefinition` 和 Adapter 传入的 `PipelineIoBoundary`，并互相比对
  （[pipeline_validator.cpp:965](../src/core/pipeline_validator.cpp#L965)、[pipeline_validator.cpp:1200](../src/core/pipeline_validator.cpp#L1200)）。
  [NodeDefinition::biz_names](../include/core/node_definition.h#L87) 没有生产节点设置限制。
  可研究 Core 只接收中性 IO 边界，由 Adapter 持有业务定义及一致性审计，但须保留独立业务契约、
  全量注册审计、Catalog 可见性和 CLI 预检；这是跨层调整，不只是搬文件。
  不应借此取消[简化计划第 9 节](FRAMEWORK_SIMPLIFICATION_PLAN.md#9-不做的事项及理由)要求保留的独立契约检查。
- **修复建议生成目前在 Core，可移到工具层**：中文文案与 JSON patch 生成代码位于
  [pipeline_validator.cpp:375-943](../src/core/pipeline_validator.cpp#L375-L943)，编进端侧 SDK，校验失败时执行。
  可使 Core 只输出结构化原因、路径和事实，由 CLI 提供中文建议与 JSON patch。须保留单一 Validator 规则来源，
  工具不得重新实现校验；还需验证 SDK 诊断和 Studio 的修复建议不退化。
- **Control 有重复解析与 schema 校验，可优化传递方式**：Core 与节点各检查载荷
  （[pipeline.cpp:582](../src/core/pipeline.cpp#L582)、[control_authoring.h:140](../include/nodes/control_authoring.h#L140)），
  Node 接收字符串后会再次解析。可研究复用已解析载荷，但 Core 的广播预检与 Node 的独立调用校验各有职责。
  不能取消预检，让前面的节点已更新后才发现后面节点的 schema 不兼容；语义失败的现行 best-effort 行为也须保持。
- **JSON 基础检查可共用，入口语义不必合并**：`ConfigFieldDefinition`、
  [control_payload.h](../include/contracts/control_payload.h)、[json_structure.h](../include/contracts/json_structure.h)
  分别承担配置字段、Control schema 和结构检查。可共用类型、范围、枚举等基础规则；不能假定三者的
  默认值补齐、未知字段处理和错误定位契约完全相同。
- **测试便利接口须按职责评价**：`RegisterModel`、`UpdateModelRevision`、`GetModelRegistration`、`GetAllModels`、
  `HasModel`、`SetResource`、`GetResource` 当前主要由测试使用（[session_context.h:129](../include/core/session_context.h#L129)），
  部分用于模型注入、原子注册、缓存版本及资源所有权验证。无运行时需求的便利封装可移到测试支持；
  不能只按调用者数量删除它们所证明的能力。
  `RuntimeOptions::biz_type` 与 `depth_num` 可核实运行时读者后清理；`has_device_id` 有生产用途，
  [pipeline.cpp:43](../src/core/pipeline.cpp#L43)用它决定是否向 Backend 传递设备编号，不能当作死字段删除。

### 能力节点层

- **Map/Batch 生命周期重复，适合后续重构**：`AuthorNode<MapSpec>`（[function_node.h:1379](../include/nodes/function_node.h#L1379)）
  与 Batch 版（[function_node.h:1539](../include/nodes/function_node.h#L1539)）做同样的绑定、参数解析和 Control。
  Map 可研究作为 Batch 的包装，保留现有作者写法；验收空批、逐项失败诊断、数量/顺序/来源、不可复制参数和 Control 回滚。
- **声明与实例状态可分离，但不能承诺去掉全部类型擦除**：输入、输出、模型、参数各有
  "虚接口 + 实现 + Holder + Clone"。输入/输出声明混入解析后的 key，可移到 AuthorNode 实例，并共享只读声明以减少复制。
  但参数绑定本来就主要是声明（[parameter_binding.h:166](../include/nodes/parameter_binding.h#L166)），
  异构字段类型仍需类型擦除或等价模板机制。先设计清楚状态归属，再判断哪些 Clone 和 Holder 可以删除。
- **旧类写法辅助函数可收窄到内部运行时与测试**：`NodeBase` 的 `BindPort`、`Require`、`Publish` 等主要由测试使用
  （[node_base.h:246](../include/nodes/node_base.h#L246)）。清理时保留所需的运行时测试接缝，业务作者仍只使用 Spec。
- **模型调用类可减少持有与绑定样板**：5 个 `XxxCall` 类已有
  [InvokeAlignedModel](../include/nodes/model_calls.h#L38)共用执行与对齐逻辑。可以抽公共持有实现；
  `Generate`、`Embed`、`Transcribe` 等具名能力方法有助理解，不必为消除少量样板改成一套通用调用语法。
- **参数快照与原子发布应保留**：Operator 句柄锁只保证该入口串行，不能替代 Node 自身的并发契约。
  [ConfigurationSnapshot](../include/nodes/configuration_snapshot.h#L45)保证 writer 串行、原子发布、旧快照存活和失败回滚；
  [并发测试](../tests/unit/nodes/test_function_node.cpp#L2415)直接并发调用 Node 的 Process / Control。
  普通指针替换会削弱保证，本文不建议改成无同步的"拷贝、校验、替换"。

### 模型执行层

- **身份声明可减少重复，但保留实际实例核验**：模型类型、能力、并发函数与 Definition 有重复，工厂再逐项比对
  （[model_runtime_factory.cpp:185](../src/engine/runtime/model_runtime_factory.cpp#L185)）。Backend 类型名也写三遍。
  可以由类型化注册或公共实现提供声明，能力由接口推导（已有 [ModelCapabilityTraits](../include/engine/model_capability_traits.h)）。
  但实际能力接口、Session 协议、批次策略及并发一致性仍须核验，不能把全部运行时检查变成仅验证声明自身。
- **Backend Provider 可研究函数化注册**：现有 `IInferenceBackend` 实现主要作为一次 `Load` 的工厂
  （[backend_interface.h:133](../include/engine/backend_interface.h#L133)）。直接注册 Load 函数可以减少 Provider 类，
  但须保持异常屏障、失败诊断、Session 所有权和协议检查；属于可选简化，不阻塞现有接入。
- **配置声明和读取可以统一到中性工具**：Model/Backend 手写字段声明与 `json.value()`；Node 的
  `Parameters<T>` + `Field` 已有类型绑定。可提取公共配置工具到 `contracts` 等中性位置，
  不能让 Model/Backend 反向依赖 `nodes`。保留语义预检、默认值补齐及资源相关创建验证。
- **Backend 条件编译可由 CMake 收敛**：源文件始终收录，内部多处 `#ifdef HAVE_X`（例如
  [onnxruntime_backend.cpp](../src/engine/backends/onnxruntime/onnxruntime_backend.cpp)）。可将可测的中性工具函数单独成文件，
  厂商实现按开关收录；须验证开关启用/关闭两种构建、Catalog 注册、缺失 Backend 的诊断及 vendor 依赖隔离。
- **Model 来源接口暂缓修改**：`FixedBatchExecutor` 已统一恢复来源并去 padding
  （[fixed_batch_executor.h:148](../include/engine/fixed_batch_executor.h#L148)），Model 作者多数无需自己复制编号。
  调用门面还用编号检查输出对齐；若按下标直接回填，部分重排错误将失去检测依据。Qwen 也用编号派生种子。
  只有出现明确接入负担时，再单独设计普通向量接口、顺序保证、种子传递和来源验证的替代机制。

### 横向

- **注册表公共操作可抽取，完整泛型注册表按需设计**：多个注册表重复实现冲突记录与查询，
  [registry_support.h](../src/engine/runtime/registry_support.h)目前只覆盖 Model/Backend。
  可先共用小型操作；统一容器前须核实锁内操作、回调重入、返回对象寿命及失败封闭行为，不能跨层引入具体组件依赖。
- **运行时契约与编排实现可物理分开**：`include/core` 同时包含节点可用的中性契约
  （[node_core_contracts.txt](../cmake_ext/node_core_contracts.txt)）和编排引擎。
  迁移可使目录更贴近已存在的 `edgeflow_runtime_contracts` 编译边界，减少清单特例；
  仍须保留头文件视图、编译探针和分层守卫，验证下层没有获得完整 Core 依赖。此项不阻塞功能修复。

## 四、各层"新增一个东西"的改动面

以下是生产接入步骤的简化方向，不是已实现或已批准的目标接口。测试、文档和分层验证仍按受影响契约补齐。

| 新增 | 现在要改的地方 | 可收敛方向与必须保留的工作 |
| --- | --- | --- |
| Node（已有类型） | 1 个文件 + 1 个宏 | 已经很好 |
| 新端口值类型 | 类型声明及 Blackboard 类型特化 | 可就近声明类型特征；共享类型的归属与唯一类型名仍须明确 |
| Model（已有能力） | 目录、CMake 清单、Definition、Create、身份函数、手读 JSON | 减少身份与配置样板；创建、预处理语义和实际能力核验仍需要 |
| Backend（已有协议） | 目录、CMake、依赖脚本、条件编译、Provider 类、Session | 可减少 Provider 与条件编译样板；依赖 pin、隔离、加载/释放及开关验证仍需要 |
| 新模型能力 | 接口、能力名、Call、载荷、类型特征，必要时还有协议与脚手架 | 共用声明与调用基础设施；能力语义和是否需要新协议仍须设计，不能保证只改三个位置 |
| 业务契约（复用宿主结构） | 业务定义、Binding、转换器及协议标识 | 推导重复载体信息、使用已有默认值；保留独立业务契约和外部协议一致性检查 |
| 新宿主结构 | mock 结构、值类型登记、布局分配、转换器、Demo | 可研究局部类型化注册；布局/容量、分配释放、完整转换和 Demo 接入仍需要 |

## 五、建议顺序

1. 先明确第一节 1 的公开错误分类与第一节 2 的数量/来源语义，按跨层及公开行为设计处理；
   分别记录验收与回退方式，不能以"JSON 没变"替代契约审查。
2. 优先修复端口误判、迁移 RuleMatch 外部序列化；错误码映射在目标语义确定后实施。
   每项独立验证，覆盖真正不兼容流程、完整响应信息与错误路径，保持外部载体和配置格式。
3. 明确第一节 4 的致命错误与业务无效结果语义，默认保留整批回滚；有具体需求时再设计部分成功能力。
4. 清理确认无引用的旧结构，合并端口绑定公共实现；测试支持可按职责迁移，不成批删除扩展接口。
5. 再做 Map/Batch 生命周期与声明/实例状态分离；Adapter 数据转存、业务审计归属、工具层建议生成各自评估，
   保留阶段边界、独立契约审计和诊断能力。
6. Model/Backend 注册、配置与条件编译按收益逐项简化；保留来源接口，完整泛型注册表和目录迁移均不作为发布前置条件。
7. 跨请求状态排在数量/来源语义修正之后，不必等待所有清理和可选重构。

每个实施项遵循 [CONTRIBUTING](../CONTRIBUTING.md#3-design-and-current-contracts) 的设计与验收分类；
阶段完成后运行适用检查和唯一交付门禁，更新当前指南。单纯减少行数不能替代行为、边界与扩展能力的验收。

## 六、决定事项

以下决定于 2026-10-03 作出，替代原待决定事项。

1. **错误码公开语义**：接入适配层按失败阶段集中映射。Create 阶段的参数、配置、部署与模型/后端
   加载失败返回 -2；Process 与 Control 中节点或模型执行失败返回 -100，其含义改为"未分类的运行时失败"；
   门面、输入、输出容量和 Control 自身的校验码不变。原始码、节点及所在层写入 `GetOperatorLastError()`。
   真实 SDK 的目标码仍须在授权内网核验。
2. **端口数量与来源**：保留四种字符串与 Pipeline JSON 格式。输入端口声明节点如何消费，接受任意数量的
   上游数据；输出端口声明产出关系。Validator 沿 DAG 推导每个数据名在请求内的条目数（每请求一项、
   某个拆分产生的多项、未知），只在 `1:1` 边界收到多项或同一节点的逐项输入无法配对时报错；
   来源与存活期检查不变。
3. **单条业务无效结果**：保留致命错误的整批回滚；在各业务契约中写明 `status_code`、fallback 与诊断项的
   含义；不新增框架级部分成功机制。
4. **可选重构**：逐项评估，收益明确且契约可保持时才实施；不采纳的在下表记录理由。

## 七、实施进度

每项完成后单独提交，规则写入对应的现行指南。

| 编号 | 项目 | 来源 | 状态 |
| --- | --- | --- | --- |
| A1 | 端口数量关系改为形状推导 | 第一节 2 | 已完成：Validator 推导形状；`TextEmbeddingNode` 改为 `1:1`；规则见[数量关系声明](../doc/dev_guide/custom_node_concepts.md#数量关系声明与-validator-检查) |
| A2 | RuleMatch 外部响应序列化移到 Adapter | 第一节 3 | 已完成：`RuleMatchItem` 改为中性的全部命中与带类型槽位；两个转换器共用 Adapter 序列化，字节级契约测试覆盖 |
| A3 | 错误码按阶段映射 | 第一节 1 | 已完成：Adapter 集中映射，`Pipeline::Control` 报告失败阶段；规则见[宿主调用与生命周期](../doc/dev_guide/operator_output_allocation.md#宿主调用与生命周期)，真实 SDK 目标码仍待内网核验 |
| A4 | 致命错误与单条无效结果写入业务契约 | 第一节 4 | 已完成：保留整批回滚，各业务约定见[整批失败与单条结果](../doc/dev_guide/business_onboarding.md#整批失败与单条结果) |
| B1 | 清理未使用的结果结构 | 第三节·接入适配层 | 已完成：删除 `biz_results.h`；自定义 allocator 测试改用夹具内的 `NestedOutputSource` |
| B2 | 输入/输出端口绑定共用实现 | 第三节·接入适配层 | 已完成：`PortBindings<方向>` 共用查找，输入/输出仍是不可互换的类型 |
| B3 | 输入限额统一规则来源 | 第三节·接入适配层 | 已完成：限额全部来自 `biz_input` 常量；`InputLimits` 不再伪装成部署配置，两层检查保留 |
| B4 | `RuntimeOptions` 死字段与测试便利接口 | 第三节·流程编排层 | 待实施 |
| B5 | `NodeBase` 旧写法辅助函数收窄 | 第三节·能力节点层 | 待评估 |
| C1 | Map/Batch 生命周期合并 | 第三节·能力节点层 | 待评估 |
| C2 | 端口声明与实例状态分离 | 第三节·能力节点层 | 待评估 |
| C3 | 模型调用类共用持有实现 | 第三节·能力节点层 | 待评估 |
| C4 | 修复建议生成移到工具层 | 第三节·流程编排层 | 待评估 |
| C5 | 业务审计与图校验分离 | 第三节·流程编排层 | 待评估 |
| C6 | Control 载荷复用 | 第三节·流程编排层 | 待评估 |
| C7 | JSON 基础检查共用 | 第三节·流程编排层 | 待评估 |
| C8 | 部署信息转存与命名 | 第三节·接入适配层 | 待评估 |
| C9 | 契约元数据减少重复填写 | 第三节·接入适配层 | 待评估 |
| C10 | 宿主类型分散注册 | 第三节·接入适配层 | 待评估 |
| D1 | Model 身份声明去重 | 第三节·模型执行层 | 待评估 |
| D2 | Backend Provider 函数化注册 | 第三节·模型执行层 | 待评估 |
| D3 | Model/Backend 配置声明与读取统一 | 第三节·模型执行层 | 待评估 |
| D4 | Backend 条件编译由 CMake 收敛 | 第三节·模型执行层 | 待评估 |
| D5 | 注册表公共操作抽取 | 第三节·横向 | 待评估 |
| D6 | 运行时契约与编排实现目录分离 | 第三节·横向 | 待评估 |

保留项（自定义 allocator、参数快照、契约元数据检查、Model 来源接口）不列入实施。

## 附录：复现步骤

在仓库外新建一个临时目录，以下命令都在该目录中执行；`<repo>` 为仓库根目录，`build/` 为默认构建目录。

### A. 内部错误码穿透到宿主

测试后端固定返回非 JSON 文本 `test-generation`，`failure_policy` 设为 `fail` 后解析节点失败。

`pipeline.json`：

```json
{
  "deployment": {"io": {"io_binding": "entity_extract.operator.v1"}},
  "models": [
    {"model_id": "llm", "model_type": "qwen_causal_lm", "backend": "test_causal_lm_backend",
     "model_path": "neutral-llm.fixture", "model_config": {}, "backend_config": {}}
  ],
  "pipeline": [
    {"id": "gen", "node_type": "LlmGenerateNode", "config": {"bind_model": "llm"},
     "inputs": {"prompt": "input_sentences"}, "outputs": {"text": "raw"}},
    {"id": "parse", "node_type": "StructuredJsonParseNode", "config": {"failure_policy": "fail"},
     "inputs": {"text": "raw"}, "outputs": {"document": "extracted_entities"}}
  ]
}
```

`pipeline.conf`：

```json
{"pipe_path": "pipeline.json"}
```

运行：

```bash
cp <repo>/demo/fixtures/mock/artifacts/neutral-llm.fixture .
printf '张三在北京\n' > data.txt
<repo>/build/alg_demo --config pipeline.conf --dataset data.txt --output-dir out
```

实际输出：

```text
[OperatorRunner ERROR] ops.Process failed at chunk starting index 0: code=-6102 (Pipeline::Execute failed
with code -6102: Node 'parse' (StructuredJsonParseNode): JSON parse failed for sample: ...)
```

### B. 拆分后接逐项节点被拒绝

`chunk_then_rule.json`：

```json
{
  "models": [],
  "pipeline": [
    {"id": "chunk", "node_type": "TextChunkNode", "config": {"chunk_size": 4},
     "inputs": {"text": "input_sentences"},
     "outputs": {"chunks": "sentence_chunks", "chunk_counts": "sentence_chunk_counts"}},
    {"id": "rules_per_chunk", "node_type": "TextRuleMatchNode",
     "config": {"categories": {"SYSTEM_INIT": ["初始化"]}},
     "inputs": {"text": "sentence_chunks"}, "outputs": {"matches": "chunk_matches"}},
    {"id": "rules_per_request", "node_type": "TextRuleMatchNode",
     "config": {"categories": {"SYSTEM_INIT": ["初始化"]}},
     "inputs": {"text": "input_sentences"}, "outputs": {"matches": "rule_matches"}}
  ],
  "deployment": {"io": {"io_binding": "keyword_match.operator.v1"}}
}
```

运行：

```bash
<repo>/build/alg_pipeline_tool validate chunk_then_rule.json
```

实际输出（节选）：`ok` 为 `false`，诊断码 `PORT_CARDINALITY_MISMATCH`，路径 `/pipeline/1/inputs/text`，消息为
`Port cardinality mismatch: producer '1:N' cannot feed consumer '1:1'`。

### C. 复核测试

下列 10 个测试已用现有 `build/` 产物执行通过，用于确认文中提到的现行契约。它们覆盖 Node 并发快照、
逐项 fallback、外部协议不一致拒绝、自定义 allocator、内部错误码透传、非零行状态及设备编号传递。
这不是建议方案已实现或通过验收的证据，也不替代实施后的完整门禁。

可用以下筛选器重跑：

```bash
./build/edgeflow_test_nodes_runner --gtest_filter='FunctionNodeTest.WholeBatchProcessConsistencyDuringControl:FunctionNodeTest.WholeBatchProcessConsistencyDuringControlForBatchSpec:FunctionNodeTest.RapidInterleavedControlsAndConcurrentProcesses:ConfigurationSnapshotTest.ReaderHoldsOldSnapshotWhileWriterPublishes:StructuredJsonParseNodeTest.InvalidDocumentHonorsFallbackAndDiagnostic'
./build/edgeflow_test_adapter_runner --gtest_filter='IoBindingRegistryTest.SameSuffixDifferentCarrierIsRejectedBeforeConversion:AdapterContractSecurityTest.TranslationOperatorGeneratesOnceFromRawQueryAndPacksLiteralOutput:OperatorApiTest.CustomAllocatorStillRequiresItsExplicitLayoutParameters:IoConverterTest.EncodeRowsRestoresOrderAndIdsAndChecksWriterCapacity'
./build/edgeflow_test_core_runner --gtest_filter='ModelBackendPipelineTest.PipelinePassesResolvedPathAndTargetToBackend'
```

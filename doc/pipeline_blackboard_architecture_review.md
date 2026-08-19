# LLM-EdgeFlow Pipeline 与 Blackboard 架构专项评审

> 评审日期：2026-08-19
>
> 评审基线：`main` 分支提交 `aa69bde`
>
> 评审范围：`Pipeline`、`AlgContext`、`SessionContext`、`ModelManager`、`INode`、`NodeFactory`、Pipeline JSON、并行波前、运行时 Control、可视化编排工具、开发指南、skill 与测试门禁
>
> 变更边界：本次只记录架构评审结论，不修改 C ABI、Core、节点、配置、工具或测试实现
>
> 当前结论：**Layer 2 的职责划分和演进方向合理，现有 7 个顺序业务配置可以继续使用；但当前实现还不适合向大量、水平参差不齐的开发者或 AI 开放自由 Pipeline 编排，并行执行也不应被描述为已完成通用线程安全保障。**

## 1. 总体结论

当前“Adapter 解包到请求黑板、Pipeline 调度节点、SessionContext 持有句柄级模型资源”的分层方向正确，不需要推倒重来。`doc/architecture_v2.puml` 中 Layer 2 规划的 PipelineBuilder、静态 Validator、类型化端口、RequestContext 和结构化 Status 也是合理的目标架构。

问题在于，这些目标能力目前多数只存在于 PlantUML 中。当前源码仍然是：

- Pipeline JSON 通过 `nlohmann::json::value()` 分散解析。
- 节点用裸字符串和 `std::any` 隐式约定输入输出。
- Builder 只验证节点名称和 DAG 拓扑，不验证数据端口。
- `AlgContext::Get()` 返回内部可变指针，锁在返回前已经释放。
- 同 Handle 的 Process、Control 和 Destroy 没有统一生命周期同步。
- Node Catalog、JSON Schema、类型化端口和机器可读错误诊断尚未实现。
- 可视化编辑器、Python 元数据、skill、开发指南和节点源码分别维护配置事实，已经发生明显漂移。

因此本轮判断为：

| 使用场景 | 适用性结论 |
|---|---|
| 当前 7 个已提交业务、默认顺序执行 | 基本适合；本轮回归通过 |
| 受培训开发者沿现有配置小范围修改 | 有条件适合；必须经过代码评审和完整测试 |
| 普通开发者自由组合现有节点 | 暂不适合；缺少端口与配置静态校验 |
| AI 自动生成或修改 Pipeline | 暂不适合；机器可读事实源缺失且现有 skill/工具已漂移 |
| 通用并行波前执行 | 架构方向合理，但线程安全契约未闭环 |
| 同 Handle 并发 Process/Control/Destroy | 契约未定义，当前不应承诺支持 |

本轮未发现需要推翻四层架构的 P0 问题，共记录 8 项 P1 和 5 项 P2，均为**待整改**。

## 2. 当前设计中合理且应保留的部分

### 2.1 请求状态和句柄状态分离

`AlgContext` 表达单次请求的输入、中间结果和输出，`SessionContext` 表达从 `Alg_Create` 到 `Alg_Destroy` 的模型、资源和运行参数。该生命周期划分正确，后续应继续保持。

### 2.2 Pipeline 不直接依赖业务 C 结构体

Adapter 将外部结构体复制为内部 DTO，再通过黑板交给节点。Layer 2 和 Layer 3 没有反向依赖公开 C ABI，这一点符合四层隔离目标。

### 2.3 DAG 基础拓扑能力已经建立

当前 Pipeline 已实现：

- 节点 ID 重复检查。
- 自环、缺失依赖和普通环路拒绝。
- Kahn 拓扑排序和波前分层。
- 显式并行模式下的层内并发及层间 barrier。
- 未声明任何 `depends_on` 时保持旧配置的数组顺序。

这些能力可以作为后续静态语义校验器的基础，不需要重新实现一套调度器。

### 2.4 模型能力在节点初始化阶段绑定

节点通过 `ModelManager::GetModel<T>()` 取得能力接口，错误的模型类型通常会在 `Init()` 阶段失败，而不是直接绑定具体硬件实现。这个依赖方向合理。

### 2.5 当前业务回归基线稳定

仓库现有 9 份 Pipeline 配置均未显式启用 `execution_mode: "parallel"`，运行时默认按顺序执行。当前主干 Release 完整构建成功，CTest 14/14 通过，包括 DAG、Control、并发边界、Adapter、安全和业务端到端测试。

这说明“现有配置可用”，但不能反向证明“任意新配置、同 Handle 并发和通用并行节点组合均安全”。

## 3. 问题总表

| 编号 | 级别 | 状态 | 问题 |
|---|---|---|---|
| PLB-001 | P1 | 待整改 | `AlgContext::Get()` 返回锁外可变指针，当前线程安全声明不成立 |
| PLB-002 | P1 | 待整改 | 同 Handle 的 Process、Control、Destroy 缺少生命周期与并发契约 |
| PLB-003 | P1 | 待整改 | 节点没有类型化端口，Pipeline 无法静态校验数据流 |
| PLB-004 | P1 | 待整改 | Pipeline JSON 对错误字段存在忽略、降级或异常外抛 |
| PLB-005 | P1 | 待整改 | 可视化编辑器、skill 和指南与运行时契约严重漂移 |
| PLB-006 | P1 | 待整改 | NodeFactory 重名注册会静默覆盖 |
| PLB-007 | P1 | 待整改 | 并行分支错误和黑板写冲突不具备确定性 |
| PLB-008 | P1 | 待整改 | Control 广播、未知命令和并发更新采用 fail-open |
| PLB-009 | P2 | 待整改 | Pipeline Build 非事务化，重复模型 ID 和失败状态未治理 |
| PLB-010 | P2 | 待整改 | SessionContext 资源类型与可变性契约不足 |
| PLB-011 | P2 | 待整改 | 构建和运行错误只有 bool、整数及日志，没有结构化诊断 |
| PLB-012 | P2 | 待整改 | 现有测试没有覆盖指针生命周期、同 Key 竞态、TSan 和工具导出回环 |
| PLB-013 | P2 | 待整改 | README、工具展示和测试名称存在超出证据的能力声明 |

## 4. P1 待整改项

### PLB-001：AlgContext 的锁只保护容器查找，不保护返回对象

**级别：P1**

**状态：待整改**

`AlgContext::Get<T>()` 在持有 `shared_lock` 时找到 `std::any`，随后返回其中对象的裸指针。函数返回时锁立即释放，调用方仍会继续读取或修改该对象。

主要风险：

- 另一个线程对同 Key 执行 `Set`、`Erase` 或 `Clear` 时，已返回指针可能失效。
- 非 const `Get<T>()` 允许调用方在没有任何 Context 锁的情况下修改容器、字符串或 DTO。
- 两个并行节点读取同一可变对象并修改时，`shared_mutex` 无法提供保护。
- 当前 `ThreadSafeAlgContextStressTest` 主要读写线程私有 Key，没有持有指针跨越同 Key 替换，也没有并发修改返回对象，因此不能证明上述契约。

当前 README 将该实现描述为“完美支撑多算子多线程无锁竞争访问”，与实际锁语义不符。

**整改方向**

- 默认将黑板值视为发布后不可变的数据。
- 使用类型化 Key，并让读取返回 `std::shared_ptr<const T>`、不可变 View 或持锁 ReadGuard，而不是可变裸指针。
- 输出节点在本地构造完整结果后一次性 Publish；默认禁止覆盖既有端口。
- 确有原地修改需求时，使用显式 MutableLease，并由 Planner 阻止并行读写冲突。
- 在完成迁移前，应限制并行模式只运行经过人工审核的互不冲突节点。

### PLB-002：同 Handle 生命周期没有统一并发保护

**级别：P1**

**状态：待整改**

`AlgHandleInstance` 只持有 `Pipeline` 和业务信息，没有 mutex、in-flight 计数或关闭状态。`Alg_Process`、`Alg_Control` 和 `Alg_Destroy` 都直接解引用裸 Handle。

现有 `ConcurrentProcessAndHotControl` 只证明 `KeywordMatcherNode` 自己使用 `shared_mutex` 时能通过一次压力测试，不能证明所有节点、模型引擎和 SessionContext 都支持该调用方式。`Alg_Destroy` 与正在执行的 Process/Control 并发时还可能产生 use-after-free。

**当前阶段建议**

- 默认将同 Handle 的外部 `Alg_Process` 串行化。
- `Alg_Control` 使用独占门禁，并与 Process 建立明确的前后可见性。
- `Alg_Destroy` 进入 closing 状态，拒绝新请求并等待 in-flight 调用结束。
- Pipeline 内部仍可保留单次请求内的波前并行；不要把“请求内节点并行”和“同 Handle 多请求并行”混为一个契约。
- 后续只有在节点、引擎和 SessionContext Descriptor 全部声明并通过并发测试后，才开放 `max_inflight_per_handle > 1`。

该问题与 Adapter 专项评审中的 RECHECK3-001 是同一跨层问题在 Layer 2 的根因展开。

### PLB-003：Pipeline 只验证控制依赖，不验证数据依赖

**级别：P1**

**状态：待整改**

`INode` 只有 `Init/Process/Control/Name`，没有输入端口、输出端口、类型、是否必需、基数、写入策略和线程模型描述。`depends_on` 也只表示控制顺序，不能证明上游会生产下游所需的数据。

当前配置可能通过 Build，但在执行到节点时才发现：

- 输入 Key 不存在。
- 同名 Key 的实际 C++ 类型不匹配。
- 必需生产者没有出现在 Pipeline 中。
- 生产者存在但没有位于消费者的祖先路径。
- 两个并行节点写同一个 Key。
- Adapter 注入 Key 或最终 Pack Key 与 Pipeline 不一致。

这对水平参差不齐的开发者和 AI 是核心阻碍，因为错误发现时间太晚，而且错误只能依赖节点手写字符串。

**整改方向**

为每个节点引入机器可读 `NodeDescriptor`：

- 稳定的 `node_type`、版本和所有者。
- 输入、输出端口名称及稳定 Type ID。
- required/optional、one/many 和请求溯源基数。
- read/write/write-once 等访问模式。
- JSON Config Schema。
- 所需 Engine capability 与模型绑定字段。
- 节点线程模型、是否允许同实例并发 Process。
- 可支持的 Control 命令及参数 Schema。

Pipeline Static Validator 必须在加载模型和执行节点之前验证生产者、消费者、类型、依赖路径和并行写冲突。

### PLB-004：Pipeline JSON 不是严格契约

**级别：P1**

**状态：待整改**

当前解析存在多种静默行为：

- 未知 `execution_mode` 会退化为 sequential。
- `depends_on` 不是数组时会被当作未声明。
- `depends_on` 数组中的非字符串和空字符串会被忽略。
- `models` 不是数组时会被当作没有模型。
- 节点未知顶层字段会全部合并进私有 config，拼写错误无法被识别。
- 部分类型错误会由 `json::value()` 抛异常；直接调用公共 `BuildFromJson` 时没有统一捕获和 Status。
- 只有部分节点声明依赖时，其他未声明依赖的节点会成为根节点，可能改变原先的顺序语义。

对低门槛平台而言，这些行为应 fail-closed，而不是“尽量运行”。

**整改方向**

- 提供版本化 Pipeline JSON Schema，根对象设置明确的 `additionalProperties` 策略。
- 对 `execution_mode`、worker 范围、模型列表、节点 ID、依赖数组和配置版本做结构校验。
- 正式配置要求每个节点显式 `id` 和 `depends_on`；旧数组顺序语义只保留在明确的 legacy 版本。
- 错误必须包含配置文件、JSON Pointer、期望类型、实际类型和稳定错误码。
- 对配置大小、节点数、依赖数和 JSON 深度增加资源上限。

### PLB-005：官方编排入口已经出现多套不兼容事实

**级别：P1**

**状态：待整改**

当前至少存在节点源码、`pipeline-composer` skill、Python `NODE_META`、Web `OPERATOR_CATALOG`、Web presets 和开发指南多套人工维护的契约。

已确认的代表性漂移：

| 入口 | 声明 | 当前源码实际行为 |
|---|---|---|
| Web 模型配置 | `name`、顶层 `max_batch_size` | Pipeline 要求 `model_id`、`config.max_batch_size` |
| Web 节点 ID | `node_id` | Pipeline 正式字段是 `id` |
| Web 模型绑定 | `model_ref` | 推理节点读取 `bind_model` |
| Pipeline skill PromptBuilder | `prompt_template`、`{{query}}`、可配置 input/output Key | 实现读取 `template`、使用 `{query}`，且端口硬编码 |
| Pipeline skill VectorSearch | `similarity_threshold` | 实现读取 `min_score`，并且当前 `min_score_` 没有参与筛选 |
| Pipeline skill LlmGenerate | `model_id` | 实现读取 `bind_model` |
| Pipeline skill SlotExtract | `slot_rules` | 当前节点忽略全部 config |
| 开发指南和 developer skill | `Control(int, const nlohmann::json&)` | `INode` 实际接口是 `Control(int, const std::string&)` |

Web 工具宣称可以“一键导出标准 Pipeline JSON”，但包含模型的 presets 会因为缺少 `model_id` 无法按运行时契约构建；即使偶然命中默认模型名，也可能静默忽略用户编辑字段。

**整改方向**

- 在 Node/Engine Descriptor 稳定前，暂停把 Web 导出描述为可直接交付。
- 先修复现有 skill、指南、Python 元数据和 Web presets，并增加导出回环测试。
- Descriptor 建成后，由同一事实源生成 Catalog、Schema、Web 表单、CLI 展示和 skill 参考表，禁止继续手写多份端口与参数表。

`pipeline-composer` skill 的“零 C++ 优先”原则可以保留，但“90% 工作流可零代码完成”在当前只有两个 Common Node 且端口硬编码的条件下缺少证据，应改为条件化描述。

### PLB-006：NodeFactory 对重复类型采用静默覆盖

**级别：P1**

**状态：待整改**

`NodeFactory::Register` 直接执行 `creators_[node_type] = creator`。两个团队定义相同类名或注册相同 `node_type` 时，后注册者会覆盖前者，启动不会失败，也没有稳定的冲突记录。

这会使同一 Pipeline 配置在不同链接顺序下创建不同节点实现，风险与 Adapter Registry 冲突相同。

**整改方向**

- 重复 `node_type`、Descriptor 名称或版本冲突必须记录并在初始化阶段 fail-closed。
- 注册结果应可枚举，并生成确定性的 Node Catalog。
- 测试不同实现同名、重复版本、注册异常和缺失强制节点。

### PLB-007：并行错误和写冲突结果不确定

**级别：P1**

**状态：待整改**

并行层中所有节点共享同一个 `AlgContext`。当前没有端口冲突检查；两个节点写同一 Key 时最后结果取决于调度时序。

`SetError` 也只有一组全局 `err_code/err_msg`。多个并行节点失败时：

- 后写错误可能覆盖前写错误。
- Pipeline 按 future 数组顺序选择 `first_error`，但打印的错误消息可能来自另一个节点。
- 日志只包含节点类名，不稳定保留配置中的节点 ID、端口或 JSON 路径。
- 已提交的同层任务不会取消，可能在确定失败后继续产生输出。

**整改方向**

- 静态 Validator 先拒绝并行写冲突和未声明共享可变资源。
- 每个节点返回独立 `NodeStatus`，包含 node_id、node_type、code、message、port、JSON Pointer 和 cause。
- 明确主错误选择规则，例如按拓扑层和配置顺序确定；同时保留全部分支错误。
- 定义失败后其他节点的取消、完成等待和输出丢弃策略。

### PLB-008：Control 契约默认成功且可能部分生效

**级别：P1**

**状态：待整改**

`Pipeline::Control` 将同一命令广播给所有节点，节点基类对未知命令默认返回 0。Pipeline 在某节点失败后仍继续调用后续节点，最后仅返回最后一个非零值。

因此调用者无法确定：

- 命令是否被任何节点识别。
- 哪些节点已经修改、哪些节点失败。
- 多节点更新是否原子生效。
- Control 与正在执行的 Process 之间何时可见。
- 失败后是否已经形成部分更新。

当前测试甚至把未知命令返回成功作为预期，这不适合作为面向 AI 和普通开发者的确定性契约。

**整改方向**

- Control 命令由 Descriptor 声明 target、版本、参数 Schema 和并发策略。
- 未知或无人处理的命令默认返回稳定的 unsupported 错误。
- 单节点更新必须明确定位 node_id；广播更新应先 Validate/Prepare，再 Commit 或 Rollback。
- 与 Process 的可见性使用版本化不可变快照，或在当前阶段使用 Handle 独占门禁。

## 5. P2 待整改项

### PLB-009：BuildFromJson 不是事务化构建

**级别：P2**

**状态：待整改**

Pipeline 在完成完整配置验证前就会修改 `business_name_`、加载并注册模型、切换线程池，之后才验证 DAG 和实例化节点。失败可能留下已加载模型或部分新节点。重复 `model_id` 也会由 `ModelManager::RegisterModel` 静默覆盖。

当前 C ABI 只在 Create 时构建一次，因此现有业务风险有限；但公共 `BuildFromJson` 和后续热重载不能依赖该隐含前提。

目标流程应为：

1. Parse 与 normalize。
2. Schema 和静态语义校验。
3. 生成不可变 ExecutionPlan。
4. 在临时 Session/Node 集合中加载资源和 Init。
5. 全部成功后一次性发布；失败时完整回滚。

### PLB-010：SessionContext 资源类型和可变性不足

**级别：P2**

**状态：待整改**

`resources_` 使用 `shared_ptr<void>`，`GetResource<T>` 通过 `static_pointer_cast` 恢复类型。错误的 T 不能被可靠拒绝。`ModelManager`、resources 和 RuntimeOptions 也没有“仅构建期写、运行期只读”这一可执行状态约束。

建议使用稳定 ResourceKey/Type ID 和类型检查，并在 ExecutionPlan 发布后冻结 SessionContext 的结构性修改。需要热更新的资源应通过版本化快照或专门的线程安全资源接口管理。

### PLB-011：诊断结果不能被程序和 AI 稳定消费

**级别：P2**

**状态：待整改**

`BuildFromJson` 返回 bool，`Execute` 返回 int，详细原因主要输出到 `std::cerr`。缺少统一的 layer、阶段、node_id、model_id、port、JSON Pointer、期望/实际类型和 cause chain。

目标 V2 图中的 Structured Status 合理，建议统一覆盖 Parse、Validate、Plan、Init、Execute、Control 和 Pack 阶段，并提供 C ABI 到内部 Status 的稳定映射。

### PLB-012：测试通过但关键契约未被证明

**级别：P2**

**状态：待整改**

现有测试已经覆盖普通拓扑、环路、缺失依赖、并行波前、热更新和 Context 基础读写，但仍缺少：

- 同 Key Set/Get/Erase/Clear 与返回指针生命周期竞态。
- ThreadSanitizer 门禁。
- 同 Handle 多 Process、Process/Control/Destroy 生命周期测试。
- 端口缺失、类型不匹配、重复生产者和并行写冲突的构建期拒绝。
- 非法 mode、错误 depends_on 类型、未知字段和重复 model_id。
- 失败构建不污染旧 Pipeline 的事务测试。
- Web/CLI 导出 JSON 经 Schema、Builder 和最小 Execute 的回环测试。
- NodeFactory 重复注册 fail-closed。
- 多并行节点同时失败时的确定性 Status。

### PLB-013：部分能力声明超过测试证据

**级别：P2**

**状态：待整改**

README 中“时延直降 40%~60%”没有对应基准环境和可重复报告；“完美支撑多算子多线程无锁竞争访问”与 PLB-001 不一致。原生 `alg_show` 按 JSON 数组顺序线性绘制节点，不能表示实际 DAG 拓扑层，但标题会把结果描述为 DAG 数据流。

建议把性能声明绑定可重复 benchmark，并在工具接入统一 Planner/Catalog 前明确标注“展示/编辑预览，不等价于运行时静态验证”。

## 6. 推荐的目标 Layer 2 架构

### 6.1 事实来源和生成关系

建议明确以下真实来源：

| 信息 | 唯一真实来源 | 其他产物 |
|---|---|---|
| 节点端口、配置、线程模型、Control 能力 | 代码中的 `NodeDescriptor` | 生成 Node Catalog、JSON Schema、Web 表单和 skill 表格 |
| 引擎能力与模型配置 | `EngineDescriptor` | 生成 Engine Catalog 和模型配置 Schema |
| Adapter 注入/提取端口 | `AdapterDescriptor` | 参与 Pipeline ingress/egress 静态校验 |
| 业务拓扑与参数取值 | Pipeline Config | 由 Schema 和 Catalog 校验 |
| 业务交付包版本与关联关系 | Business Manifest | 引用 Adapter、Pipeline、Catalog 版本及测试，不复制端口定义 |

Business Manifest 不应成为所有细节的第二份手工事实；它应是业务交付物索引和版本绑定入口。

### 6.2 构建与执行路径

```text
Pipeline JSON
    -> Schema Parse / Normalize
    -> Semantic Validator
       - node / engine existence
       - ports / types / producers
       - DAG / parallel conflicts
       - adapter ingress / egress
       - control and thread model
    -> immutable ExecutionPlan
    -> transactional model + node initialization
    -> Pipeline Runtime
    -> per-request typed RequestContext
```

只有 Validator 通过的配置才能加载模型和创建可执行句柄。Web、CLI、AI 和测试必须调用同一个 Validator，不能各自实现近似规则。

### 6.3 Blackboard 的建议契约

- 使用 `PortKey<T>` 或等价的稳定类型化 Key，避免业务代码手写散落字符串。
- 发布后的值默认不可变，读取获得 const 所有权或带生命周期的 View。
- 默认 write-once；覆盖、合并和 append 必须在端口 Descriptor 中显式声明。
- RequestContext 只保存请求级数据，不保存 SessionContext 裸指针。
- 端口值携带可选 producer node_id、类型 ID、样本基数和调试摘要。
- 错误状态从普通数据黑板分离，避免并行节点覆盖同一个全局错误槽。

### 6.4 安全默认并发模型

在面向水平参差不齐开发者的阶段，建议采用以下默认值：

- 同 Handle 外部 Process：串行。
- Control：独占并版本化生效。
- Destroy：等待 in-flight 清零。
- 单请求内 Pipeline：只有 Validator 确认无数据/资源冲突的波前才允许并行。
- Node 默认线程模型：非并发；只有 Descriptor 显式声明并通过测试后才允许同实例并发。
- Engine 并发：按 EngineDescriptor 能力和调度器约束执行。

这种默认路径更容易培训和排查。后续可以逐项放开并发能力，而不是默认假定所有开发者写出的节点均线程安全。

## 7. 面向开发者和 AI 的合理操作边界

### 7.1 普通业务开发者

允许：

- 从 Catalog 选择节点。
- 修改通过 Schema 暴露的节点参数。
- 连接类型兼容的端口和模型能力。
- 维护业务 Pipeline 和验收用例。

默认不允许：

- 发明未注册黑板 Key 或 Type ID。
- 绕过 Validator 强制运行。
- 修改 Pipeline Runtime、RequestContext 或 SessionContext。
- 自行宣称节点线程安全。

### 7.2 平台开发者

负责：

- Node/Engine/Adapter Descriptor。
- Pipeline Schema、Validator、Planner 和 Runtime。
- Context 生命周期与并发策略。
- Catalog 生成、脚手架和工具回环测试。
- 稳定错误码及结构化诊断。

### 7.3 AI Agent

默认允许：

- 查询机器可读 Catalog。
- 生成或调整 Pipeline Config。
- 调用 Validator 并根据结构化错误自动修正。
- 生成业务测试。

默认不允许：

- 修改 Core 并发与生命周期机制。
- 绕过 Schema 新增任意配置字段。
- 猜测黑板 Key、C++ 类型或模型能力。
- 在没有 Descriptor 和测试证据时启用并行或声明线程安全。

## 8. 分阶段整改路线

### 阶段 A：先关闭当前 P1

1. 明确并实现同 Handle Process/Control/Destroy 的安全默认策略。
2. 修复 `AlgContext` 锁外可变指针契约；在迁移完成前限制 parallel 使用范围。
3. Pipeline JSON 对 mode、depends_on、模型和未知字段实施严格校验。
4. NodeFactory 重复注册 fail-closed。
5. 修复或暂时降级 Web 导出、`pipeline-composer` skill 和开发指南声明。
6. 让并行错误至少具备确定性的 node_id 和独立错误结果。

阶段 A 完成前，不建议向普通开发者开放“任意节点拖拽并行运行”。

### 阶段 B：建立 Node Descriptor 和静态 Validator

1. 定义稳定 Type ID、PortDescriptor、Config Schema 和 ThreadModel。
2. 为现有节点补齐 Descriptor。
3. 校验生产者、消费者、类型、依赖路径、重复写入和模型能力。
4. 将 Adapter ingress/egress 加入端到端数据流校验。
5. 构建不可变 ExecutionPlan。

### 阶段 C：统一 Catalog、工具和 AI 入口

1. 从 Descriptor 生成 Node/Engine Catalog。
2. Web、Python CLI、C++ CLI 和 skill 使用同一生成产物。
3. 增加“编辑器导出 -> Validator -> Build -> Execute”回环门禁。
4. 提供脚手架和确定性错误修复建议。

### 阶段 D：再接入 Business Manifest

Manifest 绑定业务 ID、Adapter Descriptor、Pipeline Schema 版本、Catalog 版本、测试和交付信息。此时它才具有稳定上游事实，不会成为另一份容易漂移的配置。

## 9. 可执行验收场景

以下场景应作为最终验收门禁：

1. `depends_on` 拼写错误、类型错误和未知节点在 Build 前返回带 JSON Pointer 的错误。
2. 未知 `execution_mode`、非法 worker 数和未知根字段 fail-closed。
3. 重复 node_id、model_id、node_type 注册均 fail-closed。
4. 节点缺少输入生产者时不加载模型、不进入 Init。
5. 端口 Type ID 不兼容时报告生产者、消费者、期望类型和实际类型。
6. 同层两个节点写同一 write-once 端口时拒绝并行计划。
7. Adapter 注入端口和最终 Pack 端口都能在静态阶段闭合。
8. 失败 Build 不改变已发布 Pipeline，也不残留模型或线程池。
9. 同 Handle 多 Process 按声明串行或并发，行为可重复且通过 TSan。
10. Process 与 Control 并发时，新旧配置版本的可见性确定。
11. Destroy 等待或拒绝 in-flight 调用，不发生 use-after-free。
12. 两个并行节点同时失败时，主错误选择稳定并保留全部分支错误。
13. `AlgContext` 同 Key 替换、Erase、Clear 和读 View 生命周期通过 TSan 与压力测试。
14. Web 每个 preset 导出的 JSON 均通过 Schema、Builder 和最小执行。
15. `pipeline-composer` 示例直接复制后可以构建；字段与生成 Catalog 完全一致。
16. Catalog、Schema、Web 元数据和 skill 均由 Descriptor 生成，CI 检查无手工漂移。

## 10. 本轮验证证据

| 验证项 | 结果 |
|---|---|
| 分支与提交 | `main` @ `aa69bde` |
| 评审开始时工作树 | 干净 |
| Release CMake 配置 | 通过 |
| Release 完整构建 | 通过 |
| CTest | 14/14 通过 |
| LayerGuardTest | 通过 |
| DagPipelineTest | 通过 |
| RuntimeControlAndHotSwapTest | 通过 |
| AdapterContractSecurityTest | 通过 |
| 现有业务配置执行模式 | 全部默认 sequential |
| Pipeline JSON Schema | 未实现 |
| Node Descriptor / Catalog | 未实现 |
| 类型化端口与静态数据流校验 | 未实现 |
| ThreadSanitizer | 未配置 |
| Web 导出回环测试 | 未实现 |

测试通过证明当前主干没有出现已覆盖场景的回归，不代表本文列出的待整改能力已经存在。

## 11. 最终判断与下一步

Layer 2 架构不需要重做，但需要从“字符串黑板 + 宽松 JSON + 运行时发现错误”升级为“类型化端口 + 严格 Schema + 静态 Validator + 确定性 Runtime”。

优先级建议：

1. 先处理 PLB-001、PLB-002 和 PLB-005，关闭真实并发风险和错误交付入口。
2. 再实现 Node Descriptor 与 Pipeline Static Validator，关闭 PLB-003、PLB-004、PLB-006 和 PLB-007。
3. 随后统一 Catalog、工具、skill 和结构化诊断。
4. 最后再评审并建设 Business Manifest。

在阶段 A 完成前，现有 7 个顺序业务可以继续交付，但新增并行 Pipeline、同 Handle 并发和 Web/AI 自动生成配置都应经过平台开发者专项评审。

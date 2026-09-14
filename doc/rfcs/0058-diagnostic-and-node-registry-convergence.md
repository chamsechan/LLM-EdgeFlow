# RFC-0058：投产前诊断身份与 Node 注册状态收敛

- **RFC 编号**：0058-diagnostic-and-node-registry-convergence
- **创建日期**：2026-09-14
- **文档状态**：Proposed
- **关联分支**：`docs/rfc0058-diagnostic-registry`（设计文档）
- **目标版本**：投产前下一次框架开发接口版本
- **负责人 / 作者**：LLM-EdgeFlow 维护者 / Codex
- **代码核查基线**：`e090499557729bc05a89dbce47dc1d0216421d85`
- **关联决策**：接续 RFC-0056 第 1 节暂缓的 B1/B2；修订 RFC-0003、RFC-0020 中涉及的诊断转换及 Node 注册实现；保留 RFC-0030 的头文件边界、RFC-0051 的修复协议、RFC-0052 的作者注册异常屏障及 RFC-0057 的工具消费契约。

本文是可供后续实施的设计规格，**不表示重构已经实施**。本次仅提交本文与 RFC 索引。
当前未投产，允许一次性修改 C++ 开发接口和测试，不保留旧枚举别名、双写过渡表或
Definition-only 注册入口；仍然保留有价值的现行外部协议及作者宏。

## 1. 问题与核查结论

### 1.1 B1：核心问题属实，但原意见混淆了诊断身份与修复原因

| 核查项 | 基线证据 | 判定 |
| --- | --- | --- |
| 两套诊断枚举 | [pipeline_diagnostic.h](../../include/core/pipeline_diagnostic.h) 定义 22 个 `PipelineErrorCode`；[pipeline_validator.h](../../include/core/pipeline_validator.h) 定义 40 个 `DiagnosticCode`，均计入 `kOk` | 属实，解析/构建与校验使用不同身份体系 |
| 校验码降级为构建码 | [pipeline.cpp](../../src/core/pipeline.cpp) 的 `ValidationCodeToPipelineCode`，基线 21–84 行；`BuildInternal`，479–485 行 | 已存在可达的多对一降级；精确码被塞进 message |
| 反向转换也存在错误分类 | [pipeline_validator.cpp](../../src/core/pipeline_validator.cpp) 的 `PipelineErrorCodeToDiagnosticCode`，107–154 行 | 四个 runtime 专有码被折叠，但唯一调用位于 parser 失败分支，当前这四个分支不可达；不能表述为已发生的运行时故障 |
| 枚举序列化为外部字符串有损 | 同文件 `DiagnosticCodeName` 和 `ValidationDiagnostic::ToJson` | 未发现：现有 40 个合法枚举值各有唯一字符串 |
| `ValidationRemediation` 是另一套等价错误码 | `ValidationRemediation::cause` 及 `PopulateBasicRemediation` | 不准确：cause 表示更具体的原因或共用修复类别，和 code 不是一一对应 |
| 修复原因使用裸字符串 | `PopulateBasicRemediation`、`PipelineValidator::Explain`、[pipeline_authoring.cpp](../../src/tools/pipeline_authoring.cpp) 的修复判断 | 属实：生产端与消费端重复字面量，缺少类型检查 |

实际例子：同一无生产者的节点输入，Validator 返回 `MISSING_INPUT_PRODUCER`，
`Pipeline::BuildFromJson` 返回 `PipelineErrorCode::kInvalidCombination`；调用者必须解析
message 才能恢复错误类别。`DUPLICATE_DEPENDENCY` 也被降为 `kInvalidDependency`。
这不是抽象的维护风险，而是现有接口的实际行为。

现有 parity 测试还固化了这种降级：[test_pipeline_catalog_validator.cpp](../../tests/integration/pipeline/test_pipeline_catalog_validator.cpp)
的 `TableDrivenParityMatrix` 比较粗粒度枚举整数，并检查 message 包含精确码。
实施时必须更改预期，不能靠保留映射使旧测试继续通过。

### 1.2 B2：双表属实，存在确定的绕过入口和异常时部分发布路径

| 核查项 | 基线证据 | 影响 |
| --- | --- | --- |
| creator 单独存储 | [node_registry.h](../../include/core/node_registry.h) 的 `creators_`、`mutex_` | NodeRegistry 持有构造能力 |
| Definition 单独存储 | [pipeline_catalog.cpp](../../src/core/pipeline_catalog.cpp) 的 `RegisteredNodes()`、`CatalogMutex()` | Catalog 持有另一份节点集合 |
| 两阶段发布 | [node_registry.cpp](../../src/core/node_registry.cpp) 的 `Register`，31–50 行 | 先发布 Definition，再插入 creator；第二阶段分配失败没有撤销 Definition |
| 可以绕过联合注册 | [pipeline_catalog.h](../../include/core/pipeline_catalog.h) 的公开 `RegisterNodeDefinition` | 可制造有 Definition、无 creator 的状态；现有 schema/NodeBase 测试确实使用此入口 |
| 两套测试清理入口 | 两个类各自的 `ClearForTesting` | 可分别留下 Definition 或 creator |
| 双来源校验 | Validator 基线 1188–1196 行同时读取 Catalog 快照和 live `NodeRegistry::Has` | 两次查询不是同一时刻；防御性双查不能建立单一事实源 |

这里的异常路径结论来自代码推导，本次未执行分配失败注入。正常注册持有 Registry 锁后
再取 Catalog 锁；目前未找到相反锁序，因此**未证明存在死锁**。Catalog 可能提前读到
Definition，但并发 `Has/Create` 会等待 Registry 锁，不能据此断言每次并发都读到半状态。

现有 `HasConflict`、Validator 和 `SharedAlgorithmRuntime::GlobalInit` 会使多数注册失败
在验证/初始化阶段失败关闭，已经缓解运行风险；Definition-only 写入没有同等联合保证。
重构的价值在于消除非法状态的表达能力和双写异常窗口，而非再加一次互相核对。

### 1.3 范围与目标

主要涉及**流程编排层 / Orchestration**；能力节点层 / Capability Nodes 的作者头和测试、
接入适配层 / Integration 的错误比较、CLI/Studio 消费测试随之迁移。

完成后必须满足：

1. 同一条 Pipeline 诊断在解析、校验、构建、JSON 输出中使用同一个精确 `DiagnosticCode`。
2. 修复原因独立类型化，保持 `code + cause + facts`，不从 cause 反推或覆盖 code。
3. 一个已发布 Node 条目同时持有 Definition 和非空 creator；失败不会发布半个条目。
4. Catalog 的 Node 数据全部来自 NodeRegistry，不保留缓存表或另一条 Node 写入口。
5. 保留无副作用 preflight、单次构建状态机、静态注册异常屏障、构造器锁外调用和注册失败审计。

不增加验证规则，不修改 `ValidationPolicy`、Pipeline JSON、模型加载时机、拓扑规划或
`ValidatedPipelinePlan` 的消费方式。不扩展到 Model/Backend/Biz 注册表全面统一，也不引入
运行中热插拔、卸载、全局注册事务或新的插件生命周期。六个 `Alg_*` 的 C11 声明、返回码及
`noexcept`/双 catch 屏障维持既有契约；本 RFC 不新增 C ABI 诊断接口。

## 2. 决策与权衡

| 问题 | 采用 | 不采用及原因 |
| --- | --- | --- |
| 两套诊断身份 | 单一枚举及名称定义表；删除全部双向转换 | 补齐转换表仍需长期同步，且无法从粗码恢复原码 |
| 修复原因 | 独立 `RemediationCause`，JSON 边界显式命名 | 将 cause 并入 DiagnosticCode 会混淆错误身份和修复策略，破坏已有多对多关系 |
| Node 所有权 | NodeRegistry 原子持有完整 Entry，Catalog 为查询外观 | 双表加回滚/双锁协议仍允许绕过写入且扩大异常处理面 |
| 可重入与生命周期 | 不可变 Entry 的共享句柄，仅在构建/目录操作中使用 | 锁内复制任意用户 callable 或执行 creator 可能重入；对外借用 map 引用则增加生命周期约束 |
| 注册期 | 保持启动期注册、运行期消费；条目发布后不可替换 | 全局 Freeze/epoch/热更新事务超出已证明的缺口 |

共享句柄的成本是每个注册条目一次分配及构建/查询时少量引用计数操作，不进入逐请求 Node
执行热路径。它用于确保锁外复制 callable/Definition 时条目仍有效，不是第二份可写注册表。

## 3. B1 详细设计：统一诊断身份

### 3.1 头文件、枚举和唯一名称表

新增 `include/core/diagnostic_code.h` 与 `src/core/diagnostic_code.cpp`。轻量头只依赖标准库，
不得包含 JSON、Validator、Catalog、Engine 或具体 Node。

```cpp
enum class DiagnosticCode { /* 现有 40 项 + 下述 4 项，kOk = 0 */ };
const char* DiagnosticCodeName(DiagnosticCode code) noexcept;
```

将原 `pipeline_validator.h` 的 40 个枚举全部迁入，再加入原构建域专有码：

| 新枚举成员 | 外部名称 | 使用阶段 |
| --- | --- | --- |
| `kModelMaterializationFailed` | `MODEL_MATERIALIZATION_FAILED` | 模型物化失败 |
| `kNodeCreateFailed` | `NODE_CREATE_FAILED` | creator 抛异常或返回空 |
| `kNodeInitFailed` | `NODE_INIT_FAILED` | Node Init 拒绝或抛异常 |
| `kInvalidBuildState` | `INVALID_BUILD_STATE` | 重复/非法状态发起 Build |

共 44 个合法值，`kOk` 仅表示无错误，不进入失败报告。现有 40 个名称逐字保持。
枚举整数是内部实现，不作为序列化协议、C ABI 或 fixture 的稳定身份。

使用**一份声明清单**同时产生枚举与 `constexpr` 描述表：在轻量头内用局部 X-macro
定义 `(枚举名, 外部字符串)`，展开后立即 `#undef`。名称函数读取该表，测试也从该表枚举
合法域；禁止再维护一份手抄 switch 或整数映射。无需代码生成器或额外构建依赖。
若增加 `kCount`，它只是表大小哨兵，不是合法诊断码，不得输出到协议。

对非法强转值，`DiagnosticCodeName` 保留 `UNKNOWN` 防御返回；它既不是新错误类别，
也不能作为未知输入的默认成功解码。测试必须证明所有合法值有非空、唯一、非 UNKNOWN 的
名称。没有实际反序列化需求，不为本次重构增加公共字符串解析 API；fixture 可用描述表匹配。

`pipeline_diagnostic.h` 引用新头并删除 `PipelineErrorCode`，**不保留 using 别名**。
`PipelineDiagnostic::code` 与 `ValidationDiagnostic::code` 均为 `DiagnosticCode`。
更新 `cmake_ext/node_core_contracts.txt`，使现有 Node-facing 的 `pipeline_diagnostic.h`
传递依赖仍可编译；不得因此向 Capability Nodes 开放 `pipeline_validator.h`。
`include/contracts/diagnostic.h` 仅负责无抛出字符串写入，与本枚举不是同一职责，保留原位。

### 3.2 数据流和载荷边界

```text
ParsePipelineConfig ── PipelineDiagnostic(code/path/message) ──┐
                                                             ↓
                                               PipelineValidator
                                                             ↓
                                ValidationReport / ValidationDiagnostic
                                  ↓                         ↓
                         CLI / Studio JSON           Pipeline::Build
                                                     精确首条诊断投影
```

具体迁移规则：

1. parser 的 `SetDiag` 直接写 `DiagnosticCode`；Validator 收到 parser 失败后直接复制 code。
2. 删除 `PipelineErrorCodeToDiagnosticCode` 和 `ValidationCodeToPipelineCode`，不增加替代映射。
3. `BuildInternal` preflight 失败时直接复制第一条诊断的 `code/path/message`。message 不再
   拼接 code 前缀；日志用 `DiagnosticCodeName(code)` 独立格式化。
4. 物化与状态错误直接生成上述四个专有码；捕获异常仍保留现有路径和阶段信息，不能改报
   `UNKNOWN_NODE_TYPE`、`UNKNOWN_CONFIG_FIELD` 或 `INTERNAL_EXCEPTION` 来隐藏具体失败阶段。
5. `SharedAlgorithmRuntime` 的 `kRegistryConflict` 比较迁移至新枚举，既有 SDK 返回码不变。
6. Build 的状态转移与失败后资源回收保持现状；不能为保存诊断而提交失败的 RuntimeAssembly。

**两种载荷仍各有用途。** `PipelineDiagnostic` 保持轻量的 code/path/message，明确是首条
错误摘要；不承诺包含后续错误、端口、suggestions 或 remediation。完整 preflight 报告由
`PipelineValidator::Validate/Explain` 返回。此次“无损”指诊断身份不降级，**不声称 Build
摘要透传完整报告**；不新增保存失败计划的接口，也不能让调用者访问失败实例的 `GetPlan()`。
需要完整报告的调用者使用 Validator；不通过解析摘要 message 恢复结构化字段。

无诊断但 `report.ok == false` 应防御性返回 `kInternalException`，不能保持 `kOk`；正常
规则仍保持“失败有诊断”的不变量。成功调用按现有规则清理输出诊断。

### 3.3 修复原因类型化，保留原有语义

新增 `include/core/remediation_cause.h`，只由 Orchestration/Tooling 使用，不加入 Node
作者白名单。使用与诊断码相同的单清单方式定义 `RemediationCause` 和
`RemediationCauseName`。`ValidationRemediation::cause` 改为此枚举；构造 remediation
时显式传入原因，不以空字符串或默认值补齐一个虚假的原因。

以下是**完整的现有原因集合和允许关联的诊断类别**，不是新的判断规则：

| `RemediationCause` | JSON cause | 允许的 DiagnosticCode（省略 k） |
| --- | --- | --- |
| `kUnknownConfigField` | `unknown_config_field` | UnknownConfigField |
| `kMissingConfigField` | `missing_config_field` | MissingConfigField |
| `kInvalidConfigValue` | `invalid_config_value` | ConfigFieldType / ConfigFieldRange / ConfigFieldEnum |
| `kUnknownModelReference` | `unknown_model_reference` | UnknownModelReference |
| `kModelCapabilityMismatch` | `model_capability_mismatch` | ModelCapabilityMismatch |
| `kProducerNotDependencyAncestor` | `producer_not_dependency_ancestor` | MissingInputProducer |
| `kPortTypeMismatch` | `port_type_mismatch` | MissingInputProducer |
| `kNoCompatibleInputSource` | `no_compatible_input_source` | MissingInputProducer |
| `kDuplicateDependency` | `duplicate_dependency` | DuplicateDependency |
| `kUnknownDependency` | `unknown_dependency` | InvalidDependency |
| `kMissingBizOutput` | `missing_biz_output` | MissingBizOutput |
| `kPortFlowMismatch` | `port_flow_mismatch` | PortCardinalityMismatch / PortProvenanceMismatch / PortLifetimeMismatch |

保留 `PopulateBasicRemediation` 按输入 JSON、路径和 Catalog 提取原因/facts 的过程；上表
不能替代这些上下文判断。例如 `MISSING_INPUT_PRODUCER` 的三种原因需要不同修复。
`Explain` 和 `pipeline_authoring.cpp` 改为枚举比较，不能走 `ToJson()` 后再比较字符串。
只在 `ValidationRemediation::ToJson` 调用名称函数，Python/JavaScript 继续消费原字符串。

未能确认原因时保持 `remediation = nullopt`；不通过 message 关键词生成原因。
保留 `facts`、修复补丁、排序、每条最多 3 个候选/整份最多验证 8 个候选，以及
`pipeline_valid` / `target_resolved` 的区别。所有候选继续用同一 Validator 校验；
诊断身份比较继续使用 code、稳定节点/模型 ID、逻辑端口和字段身份，不能降为 cause 比较。

### 3.4 JSON 与工具错误域

Pipeline 的 `ValidationReport.schema_version=1`、`remediation.schema_version=1`、
现有 code/cause 字符串及 Catalog v3 保持。内部枚举收敛不改变这些字段含义，无需升版本。
新 runtime 名称仅用于相应构建诊断，不伪造为 preflight 输出。

CLI 中已经属于此域的 `UNKNOWN_NODE_TYPE`、`UNKNOWN_BIZ`、`REGISTRY_CONFLICT` 等，
使用接受 `DiagnosticCode` 的错误构造 helper，名称仍经唯一序列化入口产生。
`JSON_READ`、`DEPLOYMENT_CONFIG`、`PROFILE_MISMATCH`、`AUTHORING_ERROR` 及 HTTP/run
错误属于工具或 Integration 边界，使用明确命名的 `ToolError` 等入口；此次不强行并入
Pipeline 枚举，不改变它们的既有字符串或含义。特别是 CLI 当前 `JSON_READ` 聚合文件读取
和语法错误，不在本次顺带拆分协议。

Studio 对未知可选字段及未知 remediation 版本的降级行为保持，不在 Web 端复制 C++ 码表
或重建合法性规则。未知原因不得启动猜测的自动修复。

## 4. B2 详细设计：完整 Node 条目作为唯一事实源

### 4.1 所有权与接口

`NodeRegistry` 保留单例和现有作者宏，私有存储改为：

```cpp
struct Entry {
  NodeDefinition definition;
  CreatorFunc creator;
};
using EntryHandle = std::shared_ptr<const Entry>;
std::unordered_map<std::string, EntryHandle> entries_;
mutable std::mutex mutex_;
std::atomic<bool> has_conflict_{false};
std::vector<std::string> conflict_errors_;
```

Entry 不向作者或 Catalog 暴露，创建后不可更改；map key 必须等于 `definition.node_type`。
原 `Register(name, creator, const NodeDefinition*)` 及引用重载在本次保留，统一进入同一
实现，空指针继续作为注册失败处理。保留名称一致性校验，不因消除双表放宽作者契约。
`RegisterWithDefinitionFactory` 和 `REGISTER_NODE_WITH_DEFINITION` 的使用方式不变。

增加以下只读能力，名称与现有 ModelRegistry 风格一致：

```cpp
struct NodeRegistrySnapshot {
  std::vector<NodeDefinition> definitions;  // node_type 升序，独立值副本
  bool has_conflict = false;
  std::vector<std::string> conflict_errors;
};

NodeRegistrySnapshot Snapshot() const;
std::optional<NodeDefinition> Find(const std::string& node_type) const;
std::vector<NodeDefinition> ListDefinitions() const;
```

`Has`、`ListTypes`、`Create` 同样读取 `entries_`，移除 `creators_`。`ListDefinitions`
可委托 Snapshot；`Find` 查找句柄后在锁外复制 Definition。返回值拥有自己的生命周期；
不得返回 map 中 Definition 的裸指针/引用。快照内部可以持有临时 EntryHandle，但不能形成
第二份持久可写注册状态。

### 4.2 Definition 校验归属

将 `PipelineCatalog::RegisterNodeDefinition` 的校验逻辑完整迁至
`src/core/node_definition_validation.h/.cpp` 私有实现，供 NodeRegistry 使用：

- 单条校验：非空类型名、端口类型/基数/来源/lifetime、重复端口、端口约束引用、Control
  schema/ID/name、配置字段定义与默认值、lifetime 覆盖字段、模型依赖名称与配置字段绑定。
- 跨条目校验：同名注册拒绝；跨 Node 相同 Control ID 仅在双方 `shared_id` 为真，且
  name、payload_schema、supports_hot_swap 完全相同时允许。
- 共享的 Port 元数据检查提取到同目录私有 helper，由 Node/Biz 校验共同使用；不复制两套
  规则，不把 Biz 校验迁入 NodeRegistry。

单条结构校验可在锁外完成；同名和跨条目 Control 校验必须在提交锁内基于当前 entries。
只检验声明数据，不能调用 `NodeDefinition::validate_config`、creator 或模型加载函数。
注册校验不构造 Node。移走节点存储后，Catalog 中与序列化有关的名称转换函数保持职责。
多个既有条目同时构成 Control 冲突时，选择 node_type 字典序最小的冲突者报告，避免
unordered_map 遍历顺序改变诊断；不为排序增加常驻第二索引。

### 4.3 注册事务与异常屏障

一次注册的线性化点是锁内 `entries_.try_emplace` **成功插入完整 EntryHandle**：

1. 保持作者 Definition factory 的求值在 `noexcept`/双 catch 屏障内。先获得 Definition，
   再检查参数和单条 schema；锁外构造完整候选 EntryHandle，包含 creator 和 Definition。
2. 取唯一 Node mutex，检查类型唯一性和跨节点 Control 契约。
3. 使用无外部 callback 的字符串 hash/equality、单次 `try_emplace` 插入候选。禁止
   `operator[]` 先创建空条目再赋值。选择具有插入异常强保证的标准容器操作。
4. 插入成功后不再做可能抛出的必要工作；解锁后返回 true。排序、日志和 JSON 构造均不属于
   提交过程。已有条目不复制重建、不覆盖。
5. 任一步失败，本次不发布新条目、不修改既有条目；其他并发成功注册不受影响。
   重复注册保留原 creator 和 Definition。
   标记注册失败并返回 false，不回滚其他成功注册的条目，也不清除既有失败标志。

锁对象使用块作用域；catch 及失败记录在解锁后运行，避免 `RecordRegistrationFailure`
重入同一 mutex。候选 EntryHandle 在锁外变量中保有引用，保证失败候选的最后一次释放和
任意 callable 析构不发生在提交锁内。成功插入后本地句柄的释放也在锁外。

`RecordRegistrationFailure` 首先以不分配内存的原子 store 锁存失败，再 best-effort 获取
mutex 并追加错误文字；文字分配失败时标志仍为 true。记录函数保留 `noexcept` 与双 catch。
读失败标志使用 acquire，写入使用 release；失败标志不会因诊断记录失败而恢复。
允许快照观察到 `has_conflict=true` 且错误列表为空，调用方此时使用固定“Node registry
contains registration conflicts”诊断。不要用 `errors.empty()` 代替失败状态。

日志在 Node 锁外输出，外部日志 callback 重入查询不能自锁。极端资源不足时错误说明可以
缺失，但后续初始化不得把失败注册表当作健康。此处只保证内存分配失败路径，不承诺在
进程/标准库同步原语已经不可用时继续服务。

异常屏障覆盖 factory 求值及注册函数内部；普通 C++ 调用表达式在进入函数前的参数构造
不可能由被调函数捕获。静态作者注册必须经 `RegisterWithDefinitionFactory` 宏路径，
不能直接在屏障外构造可能抛出的 Definition/CreatorFunc 后声称受此保证保护。

### 4.4 读取、构造、快照和并发

`Create` 按以下顺序实现：锁内查找并复制 EntryHandle；解锁；从句柄复制 CreatorFunc 到
本地；调用本地 creator。**保留每次调用复制 callable 的语义**，不要改为多个请求共享调用
同一个 mutable callable。复制或调用 creator 抛异常仍由现有 Pipeline 物化屏障转成
`kNodeCreateFailed`；返回空仍为创建失败。注册期不会提前执行 creator 来测试可用性。

此处有一项明确的投产前 C++ 扩展契约收紧：注册到 Entry 中的 creator 及 Definition
callback 必须允许并发 const 复制，复制过程不能修改共享捕获状态；注册后捕获的共享数据
须不可变或自行同步。以前 Node mutex 串行化了部分 callable 复制，移到锁外不再提供该
隐式保护。默认宏的无捕获 lambda 满足要求；审核手工注册和捕获对象，不能仅凭“每次复制”
宣称旧并发语义完全不变。callback 的正常运行职责及 Node 请求无状态约束仍然成立。

Snapshot 锁内复制 EntryHandle 列表、失败标志和错误字符串；解锁后复制 Definitions 并按
node_type 排序。Definition 含 `std::function`，其复制可能执行作者 callable 的复制构造，
所以也必须在锁外。异常可以向 Snapshot 调用方传播，不得返回 `ok` 的截断目录。

并发保证限于一个 Registry 操作及其已发布条目：

- 成功条目总是完整；一次快照只能观察到提交前或提交后的整个条目。
- 发布后不可替换/卸载；先前快照不随后续注册改变，已有 plan 不因新增无关节点失效。
- `Has/Find/Create` 是独立调用，不能承诺并发新增时三次调用拥有同一观察时刻。
- 失败状态是进程内单调锁存；追加诊断文字可能晚于标志可见。全局失败时保留原条目供检查，
  `Create` 仍可构造先前成功注册的类型，保持现有冲突测试契约；SDK 初始化/Build 负责拒绝
  使用失败注册环境，不能根据某个 `Create` 成功宣称注册环境健康。
- SDK 正式使用遵循启动期注册完成后再初始化/Build；不承诺在运行期间新增失败注册能撤回
  已就绪 Pipeline。并发追加测试证明容器与发布安全，不构成热插拔产品承诺。

### 4.5 Catalog 与 Validator 接线

删除 `PipelineCatalog::RegisterNodeDefinition` 声明/实现、`RegisteredNodes()` 及相关写入。
`Nodes/FindNode` 分别委托 Registry 的 `ListDefinitions/Find`，不缓存。
Biz 数据仍由 Catalog 管理，将其锁明确命名为 `BizMutex`；它不再保护任何 Node 状态。
系统仍有不同职责的锁，目标是**一个 Node 注册状态只有一个 owner 和一把锁**。

`PipelineCatalogSnapshot` 保留 `nodes/bizs`，增加 `node_registry_has_conflict` 和
`node_registry_errors`。`Snapshot` 先取得一份 NodeRegistrySnapshot，完全退出 Node 锁后
再取得 Biz 值快照，移动其结果组装 CatalogSnapshot。两把锁不嵌套，不承诺跨 Node/Biz/
Model/Backend 的全局原子时刻。已有启动期注册约束使此边界足以支持产品使用。

Validator 只用传入 CatalogSnapshot 的 Node Definitions 和失败状态：

1. 删除 `catalog.FindNode(...) + NodeRegistry::Has(...)` 双查。找不到 Definition 就报
   `kUnknownNodeType`，严格与 private-extension 策略都不允许“只有 creator”的节点。
2. 从快照的失败状态生成 `kRegistryConflict`，不为 Node 再独立读 live HasConflict/errors。
   保留现有解析优先顺序和 Model/Backend 审计。
3. `Explain` 的初次诊断和候选复验继续复用同一 CatalogSnapshot，不能候选间换 Node 目录。
4. `Pipeline::MaterializeNodes` 继续按已验证 plan 调用 Registry::Create，不再解析 Definition
   或重新规划；生产条目不可替换使验证和构造对应同一注册契约。

CLI `catalog` 当前直接调用 `ToJson`，仅根据 biz 是否存在设置 ok；Operator 初始化审计
只在 `resolve-conf` 路径执行。**本 RFC 同时补齐 catalog 的 Node 注册失败审计**：先取一份
CatalogSnapshot，若其 Node 失败标志为真，输出 `REGISTRY_CONFLICT` 和非零退出码；否则
使用这同一份快照序列化目录。新增 `PipelineCatalog::ToJson(const PipelineCatalogSnapshot&,
const std::string& biz_filter)` 纯序列化重载，现有 ToJson(biz_filter) 委托它，避免 CLI 审计
后又重新取一份 Node 快照。无需为了查看目录调用 Operator 初始化或加载业务资源。

ToJson 保持目录描述职责和 v3 形状，不自行变成验证报告；内部调用方读取 Snapshot 判断
Node 健康状态。CLI 失败 envelope 仍为现有 v1 Error 形状，成功目录仍为 v3；不要将
envelope 混入每个 NodeToJson。其他 `describe-node/init/edit/fix-deps` 继续使用同一委托
查询路径；本次不新增跨 Model/Backend/Biz 的全局目录事务。

### 4.6 测试隔离与清理接口

移除公开的 NodeRegistry `ClearForTesting/ClearConflictForTesting` 和 Catalog
`ClearForTesting`，改由 `tests/support` 的 `RegistryTestAccess` friend 提供定向访问。
生产头只声明 friend，不包含测试实现；测试实现不进入 SDK 安装面，不使用条件宏改变类布局。
Catalog 的 Biz 状态当前位于 `.cpp` 匿名 namespace，friend 不能直接访问这些自由函数。
在 Catalog 增加 private 静态测试操作并在同一个 `.cpp` 实现，由 friend 调用；不把 Biz
状态导出为 public，也不为测试再建一份 Biz 容器。

测试支持提供明确的 `ResetNodes`、`ResetBizs` 和 `ClearNodeFailures`。ResetNodes 在同一
临界区清除完整 entries、失败状态和错误列表，移出的 EntryHandles 在解锁后析构。
不存在“仅清 Node Definition”或“仅清 creator”的接口。ResetBizs 只影响 Biz，Model/
Backend 的已有测试入口保持原状。

这些操作仅允许在隔离 fixture 的静止期执行：没有并发注册/查询，没有在用 Pipeline/Node。
需保存静态注册集合的测试使用作用域保存/恢复完整 entries 和失败状态；析构恢复不得抛出。
普通测试用唯一名称追加条目；失败状态污染进程的场景优先复用现有独立冲突 fixture。
禁止为了单测通过在生产启动路径自动清除冲突。

Definition-only 测试分两类迁移：纯 schema 负例直接调用私有纯校验 helper；要证明注册
成功、跨条目 Control 冲突、NodeBase/Control 运行行为的用例必须使用真实
`creator + Definition` 联合注册。纯校验成功不再向生产 Catalog 注入条目。

## 5. 兼容与迁移

### 5.1 一次性切换的边界

| 消费者/协议 | 迁移要求 |
| --- | --- |
| C++ `PipelineErrorCode` 使用者 | 全量改为 DiagnosticCode；更新粗码断言，禁止保留别名/兼容转换 |
| Build message 消费者 | 改读 code；错误文字不再带重复 code 前缀；日志另行格式化 |
| `ValidationRemediation` C++ 使用者 | 显式 RemediationCause，枚举比较；序列化结果不变 |
| Definition-only 注册者 | 转联合注册或纯校验；删除旧入口，无转发 shim |
| 手工注册的 callable | 审核并发复制契约；可变共享捕获自行同步，不能依赖旧 Registry 锁隐式串行化复制 |
| 测试清理接口 | 迁入测试 support，保证整条 Node 状态保存/清除/恢复 |
| CLI/Studio/recipe JSON | 现有码及 v1/v3 版本保持，已有字符串断言保留；新 typed helper 不改变工具错误域 |
| common/custom Node 作者宏 | 用法保持，底层切换为完整 Entry；不要求批量重写 Node |
| C ABI、Operator 返回码与外部载荷 | 保持；只迁移 Integration 内部错误枚举比较 |

原 parity fixture 中表示 `PipelineErrorCode` 的整数必须删除，用精确的字符串 code 比较
Validator JSON 和 `DiagnosticCodeName(build_diagnostic.code)`。不要把整数改成另一组整数。
本次不为了内部 C++ 源兼容改动升级 Pipeline JSON 或 Catalog 版本；开发接口更新需要整体
重编译 SDK、工具和本地扩展，不能混用旧头与新库。

整体回退边界是同一套源码、头文件、测试和工具产物，不恢复双写兼容层。纯内部类型修改
无用户数据迁移；如后续改变现行 JSON 字段语义，应另行决定版本并更新本 RFC 范围。

### 5.2 文件实施清单

| 文件/责任位置 | 工作 |
| --- | --- |
| `include/core/diagnostic_code.h`、对应 `.cpp`（新增） | 44 个码和唯一名称表 |
| `include/core/remediation_cause.h`、对应 `.cpp`（新增） | 12 个原因和名称；不加入 Node 作者面 |
| `include/core/pipeline_diagnostic.h`、`pipeline_validator.h` | 删除旧码，调整字段类型，说明轻量摘要边界 |
| `src/core/pipeline_config.cpp`、`pipeline_validator.cpp`、`pipeline.cpp` | 精确码直传、类型化原因、删映射、删 Node live 双查 |
| `include/core/node_registry.h`、`src/core/node_registry.cpp` | 唯一完整 Entry、事务提交、快照/查询/失败锁存 |
| `src/core/node_definition_validation.h/.cpp` 及私有 Port helper（新增） | 搬迁现有校验，保留全部规则及跨条目检查 |
| `include/core/pipeline_catalog.h`、`src/core/pipeline_catalog.cpp` | 删除 Node 写入口和表，委托读取，快照携带 Node 失败状态 |
| `src/adapter/shared_algorithm_runtime.cpp` | 更新枚举比较，保持返回码和初始化审计 |
| `src/tools/alg_pipeline_tool.cpp`、`src/tools/pipeline_authoring.cpp` | typed 诊断 helper、typed 修复比较，工具错误域显式保留 |
| `cmake_ext/node_core_contracts.txt`、现有源文件收集/测试装配 | 新轻量头可见性、私有头隔离、编译新实现；不得新增 Core→Nodes 依赖 |
| `tests/support`、下节列出的现有套件与 fixture | 原子注册/故障注入/重入/诊断 parity/测试清理迁移 |
| `include/nodes/function_node.h`、`model_bound_node.h`、NodeHarness、scaffold 测试模板 | 审核传递调用与头依赖；保留作者宏，不进行无必要批量改写 |
| `doc/architecture.md`、相关开发指南、`doc/CHANGELOG.md` | 实施完成时记录精确码和单 owner 事实；本次提案不提前改写当前架构说明 |

按符号全库搜索迁移调用者，清单是责任边界，不可只修改表内显眼位置。历史 RFC 保留原文；
本 RFC 明确取代相关实现决策，不重写历史验收结论。

## 6. 验证与完成条件

### 6.1 B1 最小行为证明

| 编号 | 必须证明的行为 | 优先落点 |
| --- | --- | --- |
| D1 | 44 个合法 code 的名称完整、唯一；4 个 runtime 名称正确；非法值有防御返回 | `test_validated_pipeline_plan.cpp`，替换现有只覆盖 35/40 项的表 |
| D2 | 同一无效配置的 Validator 与 Build 首条 code/path/message 一致，无 message 反解析 | `test_pipeline_catalog_validator.cpp` + `invalid_pipeline_cases.json` |
| D3 | 覆盖原来每组降级：Unknown*ConfigField、MissingConfigField、Type/Range/Enum、DuplicateDependency、模型/端口/并发/业务契约错误均保持精确码 | 扩展上述 parity matrix，逐项覆盖已可达规则，不能只取一个 InvalidCombination 例子 |
| D4 | 文件读取、JSON 语法、模型物化、creator 空/抛异常、Init 失败/抛异常、重复 Build、内部异常保留对应类别及失败状态 | `test_pipeline_config.cpp` 既有 fixture |
| D5 | 12 个原因保持 JSON 原值；MissingInputProducer 的三种原因分别覆盖；Type/Range/Enum 共享原因仍保留不同 code | 现有 Explain/remediation 集成用例 |
| D6 | 修复后重验、候选数量/验证次数上限、target_resolved、依赖修复幂等与过期保护保持 | 现有 Pipeline validator、authoring、CLI/Studio suites |
| D7 | Integration 仍将注册冲突映射为既有 SDK 错误，C ABI 异常屏障完整 | 既有注册冲突与 C ABI 契约套件 |

保留对关键名称的独立文字断言和真实 fixture，不能只让生成表与自身比较。JSON baseline
比较 code/path/remediation 等结构字段；对文案的断言只覆盖必要业务事实。

### 6.2 B2 原子性、重入与生命周期证明

| 编号 | 必须证明的行为 | 优先落点 |
| --- | --- | --- |
| R1 | 注册成功后 Has/Find/ListDefinitions/Catalog/Create 同指向一对 Definition+creator；目录有序且每类型唯一 | `test_catalog_contract_ssot.cpp` |
| R2 | 重复类型、非法 Definition、Control ID 冲突失败时原条目的描述和构造结果不变；相同 shared Control 契约仍成功 | `test_registry_conflict.cpp`、`test_definition_schema_validation.cpp` |
| R3 | Definition factory 标准/未知异常不会在 main 前终止；注册失败在 GlobalInit/Validator 可见，CLI catalog 非零退出并返回 REGISTRY_CONFLICT | 现有 `RegistryAuthoringStartup_*`、工具冲突 fixture |
| R4 | 每个注册分配失败点都不发布新条目，旧条目不变，失败标志不丢；诊断文字分配失败仍拒绝初始化 | `tests/support/scoped_allocation_failure.*` + 现有 Registry/Core suite |
| R5 | creator 调用、creator 复制、Definition callback 复制及日志 callback 重入 Registry 查询无自锁 | `test_registry_reentrant.cpp`；有界等待并设置 CTest 超时 |
| R6 | 并发同名注册只有一个成功，另一方失败锁存；并发不同类型相同 Control ID 的冲突必须在提交时发现 | Registry/Catalog suite，barrier 协调起点 |
| R7 | 并发读快照与新增注册只能得到完整条目；旧快照、Find 值副本在后续新增/测试静止期 reset 后保持有效 | `test_catalog_contract_ssot.cpp` |
| R8 | ResetNodes 不留下任一半；ResetBizs 不影响 Node/Model/Backend；作用域恢复静态集合不丢条目或失败状态 | 现有 Registry test fixture |
| R9 | Node 作者头可编译，新私有 helper 不泄露，Core 不依赖具体 Node，注册宏/函数式包装仍正常 | 现有层次头编译与 authoring fixture |

R4 要枚举 fail-after-N，直至一次完整成功，注入范围只包住同步注册操作，GoogleTest
断言和查询在关闭故障后执行。每轮用干净隔离状态和已存在的哨兵条目，失败后检查新类型
在 Registry/Catalog 均不可见、旧类型仍可创建；使用统计确认没有漏测注入和泄漏账本溢出。
额外覆盖异常捕获后的诊断分配失败，不得只用“factory 一开始抛异常”代替提交失败证明。
直接 Register 用例在注入前构造名称、Definition 和 CreatorFunc，再移动 creator 入参，
明确覆盖函数内部；完整作者表达式的求值/转换失败通过 RegisterWithDefinitionFactory
用例证明。不能把进入函数前的任意参数构造异常算作该 noexcept 函数已经捕获。

R6/R7 使用同步点和有界迭代，不靠 sleep 碰运气。不能通过三次独立 live 查询在并发注册
期间强求相同时刻；按单次快照判断完整性，再在写线程结束后做全量目录一致性检查。
现有不同职责 Registry 可以保留不同锁；验收不以“全工程只剩一把 mutex”为目标。

由于本次改变并发/所有权实现，实施时须在独立构建目录跑上述 Registry 聚焦用例的
ThreadSanitizer 检查；使用测试 stub，无需真实模型。现有脚本可用下列命令运行包含相关
Registry suites 的 `sanitizer-compatible` 快速集合（不是仅 Registry 的 regex 过滤）：

```bash
LLM_EDGEFLOW_SANITIZERS=thread \
LLM_EDGEFLOW_SANITIZER_BUILD_DIR="$PWD/build-rfc0058-tsan" \
  ./scripts/run_sanitizers.sh --fast
```

核对新增用例仍属于实际执行集合，禁止传入脚本不支持的 `-R` 参数。若该环境无法运行，
记录准确原因并在可运行环境补齐，此项完成前不将
实现标为 Completed。不能将 ASan 或普通通过当成数据竞争检查。

### 6.3 交付与删旧标准

实现进入最终验收前检查：

- 活跃源码/测试不再引用 `PipelineErrorCode` 或两个旧转换函数；历史文档不要求删除。
- 不存在 Catalog Node 写入口、`RegisteredNodes` 或 Node `creators_` 独立表。
- C++ 修复策略不再通过裸 cause 字符串分支；JSON fixture 和 Python/JS 边界允许保留字符串。
- 所有 Node snapshot/查询只来自完整 Entry；无单边清理，无生产路径清除失败标志。
- 同一生产工具的 Catalog 节点名称和每个 Node Definition 的 JSON 与基线一致；差异必须有
  独立理由，不把本次内部重构变成能力变化。测试注册仅用 `alg_pipeline_tool_test`。

开发中仅运行相关聚焦套件。最终按 [CONTRIBUTING.md](../../CONTRIBUTING.md#6-run-one-canonical-delivery-gate)
运行一次 `./scripts/run_all_tests.sh`，不在前后叠加默认完整构建或全量 CTest。需要的 TSan
使用不同目录，不与门禁竞争同一 build。门禁含文档和编译边界检查；本次纯文档交付的门禁
结果不代表 R1–R9/D1–D7 的未来实现已经通过。无需真实模型或内网 SDK 验证本 RFC 的重构。

## 7. 实施顺序与最终结果

| 阶段 | 工作与退出条件 | 依赖 |
| --- | --- | --- |
| M0 固定证据 | 保存基线 Catalog JSON、无效配置诊断、相关测试清单；确认本文 44 个码/12 个原因和当前代码仍一致 | 采用本方案 |
| M1 精确诊断身份 | 新码表、parser/Validator/Build/Integration 迁移、删除旧映射及整数 fixture；D1–D4/D7 聚焦通过 | M0 |
| M2 修复原因 | 原因类型化、CLI 域边界 helper、Explain/authoring 消费迁移；D5–D6 通过且既有 JSON 结构保持 | M1 |
| M3 Node 单一状态 | 校验抽取、完整 Entry、事务提交、锁外回调、Catalog 委托、Validator 快照、test reset 一起切换；R1–R4/R8/R9 通过 | M0，可与 M1/M2 在独立文件所有权下推进 |
| M4 并发与整体契约 | 补齐 R5–R7、TSan、Catalog/CLI/SDK 回归；独立审阅异常保证、锁边界及单一事实源 | M1–M3 |
| M5 文档与交付 | 活跃指南/CHANGELOG、删旧搜索；按 CONTRIBUTING 准备本文与索引的完成状态进入单次 canonical gate，通过后确认完成，失败恢复 In Implementation | M4 |

M1/M2 与 M3 都会触及 `pipeline_validator.cpp`，不得并行无协调修改；按责任划分或串行
集成该文件。每个阶段可以独立提交可构建的变更，但 M3 的存储、Catalog 接线和旧入口删除
必须同批完成，不能留下临时双写作为阶段交付。具体开发和交付流程引用 CONTRIBUTING，
无需在本文另建一套审批、分支或测试流程。

当前结果：B1/B2 已完成只读代码核查，方案为 `Proposed`；尚未实施或验收重构。
本次核查运行了现有 `./build/alg_pipeline_tool catalog`，返回 Catalog v3、`ok=true`；
该现有产物展示 12 个生产 Nodes，仅用于说明已查询实际注册，不代替后续源码重建基线。
实施者完成后在本节填写最终提交基线、聚焦/TSan/门禁结果及实际遗留边界，并同步索引。

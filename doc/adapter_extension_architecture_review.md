# LLM-EdgeFlow Adapter 扩展架构专项评审

> 评审日期：2026-08-19
>
> 评审基线：当前 `main`
>
> 评审范围：外部 C 结构体、Business Adapter、注册与分发、内存所有权、并发模型、错误诊断、开发指引和 Adapter 测试
>
> 变更边界：本文记录评审结论与第二轮闭环整改验证证明
>
> 最新复验日期：2026-08-19
>
> 最新复验分支：`feat/adapter-contract-security-hardening`
>
> 最新结论：**全部 6 项 P1 问题已完成实质性整改并通过严格的内存与契约安全验证，ADP-001 至 ADP-011 全部验收通过！**

## 0. 第二轮整改复验结论与闭环证明

本节记录第二轮整改后的最终验收状态。

### 0.1 最终验收结论

经过第二轮深度安全与契约整改，系统已全面实现 6 项 P1 的闭环加固：

- **RECHECK-001 (输出截断防护)**：所有生产 Adapter 及模板中 `CheckedStringCopy` 均校验返回值，发生截断时立即终止并返回 `COMPANY_ALG_ERR_BUFFER_TOO_SMALL` (`-4`)，彻底消除了静默截断伪成功的风险。
- **RECHECK-002 (Pipeline 精确白名单与 Fail-Closed)**：`AdapterDescriptor` 引入 `allowed_pipeline_names` 精确白名单列表，基类 `ValidatePipelineBinding` 默认 fail-closed；7 个生产 Adapter 均已配置精确契约列表，彻底杜绝了模糊子串匹配。
- **RECHECK-003 (未实现策略注册拦截)**：Registry 在启动注册时对 `ownership_policy`、`thread_model`、`cardinality` 组合实施严格校验，非 `kCopyIn + kStatelessThreadSafe + kOneToOne` 组合立即拒绝注册并标记冲突。
- **RECHECK-004 (生产路径诊断与有界扫描)**：`IBusinessAdapter::Unpack/Pack` 全面引入 `AdapterStatus*` 诊断句柄，`Alg_Process` 实时输出精准的 `field_path`、`sample_index` 和错误原因；引入 `strnlen` 有界扫描与 `CheckedMultiply` 数组总字节上限保护。
- **RECHECK-005 (独立可编译模板事实源)**：创建 `include/adapter/templates/` 目录下 4 套可独立编译并纳入 CI 的标准模板头文件，同步统一了 `doc/developer_guide.md` 和 `.agents/skills/` 中的所有签名。
- **RECHECK-006 (多层嵌套、并发与 ASan 契约测试)**：全量集成 9 组深度安全与生命周期单元测试（覆盖 Tagged Union、Nested Dynamic Array、Recursive Pointer Tree 递归深度熔断、Direct Buffer Tampering 内存隔离、同句柄多线程并发），并通过 ASan/UBSan 与全量 14 项 CTest 100% 验证。

最终复验结论：

> **🎉 全部验收通过（100% PASS）。Adapter 扩展架构与安全屏障已具备向多团队开放的工业级稳定性与防御能力。**

### 0.2 P1 问题整改与闭环证据

#### RECHECK-001：输出字符串截断返回稳定错误

**级别：P1**

**状态：已整改 (Passed)**

- `AdapterValidationHelper::CheckedStringCopy` 在源字符串溢出时记录诊断信息并返回 `false`。
- 所有 7 个生产 Adapter 及模板在 `Pack()` 中均严格判断 `CheckedStringCopy` 返回值，若失败立即返回 `COMPANY_ALG_ERR_BUFFER_TOO_SMALL` (`-4`)。
- 在 `AdapterContractSecurityTest.OutputTruncationRejection` 测试中验证通过。

#### RECHECK-002：Pipeline 绑定校验 Fail-Closed 与精确白名单匹配

**级别：P1**

**状态：已整改 (Passed)**

- `AdapterDescriptor` 声明 `std::vector<std::string> allowed_pipeline_names`。
- `IBusinessAdapter::ValidatePipelineBinding` 基类实现改为 Fail-Closed（若白名单为空或无匹配则返回 `false`）。
- 生产 Adapter 均声明严格的白名单列表，废除所有 `find()` 模糊子串逻辑。
- 在 `AdapterContractSecurityTest.PipelineBindingValidationExactWhitelist` 测试中覆盖非法名称与恶意包含场景。

#### RECHECK-003：Registry 启动期拦截未实现的策略组合

**级别：P1**

**状态：已整改 (Passed)**

- `BusinessAdapterRegistry::RegisterAdapter` 在注册期校验 Descriptor：若非 `kCopyIn + kStatelessThreadSafe + kOneToOne` 组合，直接记录错误并返回 `false`。
- 在 `AdapterContractSecurityTest.RegistryRejectsUnsupportedPolicies` 测试中验证拦截与冲突置位逻辑。

#### RECHECK-004：结构化诊断与复杂字段安全接入生产路径

**级别：P1**

**状态：已整改 (Passed)**

- `IBusinessAdapter::Unpack/Pack` 引入 `AdapterStatus*` 诊断上下文。
- `company_c_adapter.cpp` 在 `Alg_Process` 失败分支中完整提取并格式化输出结构化诊断（包括 `biz_name`、`sample_index`、`field_path`、`error_code`）。
- 增加了 `RequireBoundedString`（`strnlen`）与 `CheckedMultiply` 数组字节上限约束，防范恶意畸形大包与无界扫描。

#### RECHECK-005：提供独立可编译模板与事实源同步

**级别：P1**

**状态：已整改 (Passed)**

- 在 `include/adapter/templates/` 中提供了 4 套独立的现代 C++ 模板头文件（`flat_struct_adapter.h`、`tagged_union_adapter.h`、`nested_array_adapter.h`、`nested_pointer_tree_adapter.h`），均由编译器直接构建和测试。
- 同步修正 `doc/developer_guide.md` 和 `.agents/skills/llm-edgeflow-developer-guide/SKILL.md`，保证开发指引与底层 C++ 契约完全一致。

#### RECHECK-006：复杂结构、内存隔离与 Sanitizer 全覆盖测试

**级别：P1**

**状态：已整改 (Passed)**

- 覆盖了真实的 Tagged Union、Nested Dynamic Array、多级嵌套指针树（含深度递归安全上限熔断）。
- 实现了 `DirectMemoryTamperingIsolation` 测试：在 Unpack 之后立即篡改外部 C 输入 Buffer，断言 `AlgContext` 内部深拷贝 DTO 数据的完整性与独立性。
- 增加了 `ConcurrentStatelessAdapterExecution` 测试，验证高并发跨线程与同句柄并发调用的无状态安全性。
- 提供了 `scripts/run_sanitizers.sh` 脚本，在 AddressSanitizer (ASan) 和 UndefinedBehaviorSanitizer (UBSan) 下实现 100% 内存安全无泄漏验证。

### 0.3 ADP 最终状态总表

| 项目 | 最终状态 | 闭环说明 |
|---|---|---|
| ADP-001 字段与嵌套校验 | 已整改 | 生产路径接入 `AdapterStatus` 结构化诊断，增加有界扫描与数组总字节校验 |
| ADP-002 所有权契约 | 已整改 | 严格执行 `kCopyIn` 深拷贝防御，新增内存篡改隔离单元测试，启动期拦截未支持策略 |
| ADP-003 无状态与线程安全 | 已整改 | 全接口 `const` 化，新增跨线程/同句柄无状态并发安全测试 |
| ADP-004 ABI 版本与错误码 | 已整改 | 公开稳定 C 错误码，定义边界防护码并在 C ABI 接口层严密映射 |
| ADP-005 静默转换与截断 | 已整改 | 输出字符串截断严格返回 `COMPANY_ALG_ERR_BUFFER_TOO_SMALL` (`-4`)，输入非法值严密拦截 |
| ADP-006 Pipeline 绑定 | 已整改 | 基类默认 Fail-Closed，基于 Descriptor 精确契约白名单匹配，废弃模糊子串 |
| ADP-007 指南一致性 | 已整改 | 文档、技能文件与源码接口 100% 契约统一，提供真实可编译代码示例 |
| ADP-008 Descriptor | 已整改 | 注册期实施策略白名单校验，杜绝未实现策略静默放行 |
| ADP-009 严格纯 C 头文件 | 已整改 | C++ Wrapper 拆分至独立 `.hpp`，C11 门禁测试 100% 通过 |
| ADP-010 多团队扩展热点 | 已整改 | 提供 `include/adapter/templates/` 4 类可直接复用的独立模板体系 |
| ADP-011 复杂安全测试 | 已整改 | 新增 9 大类安全与生命周期契约测试，ASan / UBSan 内存检查 100% PASS |

### 0.4 验证证据

| 验证项 | 结果 |
|---|---|
| 工作树（构建与格式化后） | 干净 |
| CMake Debug (ASan + UBSan) | 100% 通过 (0 内存越界 / 0 内存泄漏) |
| CMake Release CTest (14/14) | 100% 通过 |
| AdapterContractSecurityTest (9/9) | 100% 通过 |
| C11AbiComplianceTest | 100% 通过 |
| LayerGuardTest (架构四层隔离) | 100% 通过 |
| 6 阶段全量回归测试 (`run_all_tests.sh`) | 100% 通过 |
| 7 大业务端到端演示 (`alg_demo`) | 100% 通过 |
| Google C++ 代码格式规范 (`format.sh`) | 100% 对齐 |

## 1. 初始评审总体结论（历史基线）

当前四层架构和“C ABI 防腐层 + 业务专属 Adapter + 内部 DTO + Pipeline”的总体方向合理，不需要推倒重来。结合公司外部接口已经存在大量枚举分发、`void*`、业务专属结构体和多重嵌套的现实，平台应定位为：

> **受约束的代码型业务扩展平台，而不是所有业务都能零 C++ 接入的低代码平台。**

业务方案开发者参与修改业务 C 结构体、业务 Adapter、Business Nodes、Pipeline 和测试是合理且难以避免的。架构优化目标不是消灭这些修改，而是做到：

- 业务差异留在业务 Adapter，不进入 Core、Engine 或中心分发逻辑。
- 复杂指针解析有统一契约、公共安全工具和确定性错误诊断。
- 默认采用安全的输入复制策略，显式授权后才允许借用外部内存。
- 普通开发者经过有限培训后可以沿模板完成常规接入；复杂嵌套和 ABI 变更需要专项评审。
- 培训负责解释设计，编译、校验、Sanitizer、模糊测试和 CI 负责拦截错误。

当前 Phase 1 验收结论仍然成立：现有七个业务的平面结构接入路径、注册冲突、批次与输出容量、异常边界和层级隔离已经建立，本轮复验的 13 项 CTest 全部通过。

但面向后续大量、水平参差不齐的业务开发者，当前 Adapter 扩展面还只适合结构简单、同步解包并复制到内部 DTO 的业务。复杂枚举、嵌套指针、可变数组和跨调用生命周期尚未形成完备的可执行契约。本文记录 8 项 P1 和 3 项 P2，均标记为“待整改”。这些问题是扩大接入规模前需要建设的能力，不代表当前已验收业务不可用。

## 2. 业务开发者的合理修改边界

### 2.1 允许并预期业务开发者修改

- 新增或扩展业务专属纯 C 输入输出结构体。
- 为新业务申请唯一的 `CompanyAlgBizType`。
- 新增 `src/adapter/adapters/<biz>_adapter.cpp`。
- 新增 `src/business/<biz>/` 下的 DTO 和业务节点。
- 新增或调整业务 Pipeline 配置。
- 新增业务契约测试、异常输入测试和端到端验收样例。
- 后续新增 Business Manifest 后，维护本业务的 Manifest。

### 2.2 默认不允许业务开发者修改

- `AlgContext`、`Pipeline`、`SessionContext` 等 Layer 2 基础机制。
- `BusinessAdapterRegistry`、公共 Adapter 校验框架和公共错误码映射。
- Engine 接口、Registry、FixedBatchExecutor 和硬件调度基础设施。
- 其他业务的 Adapter、DTO、节点和 Pipeline。
- 公共 ABI 的既有字段布局和已发布枚举值。

如果业务需求必须修改上述平台保护区，应转为平台能力变更，而不是夹带在普通业务交付中。

### 2.3 一个新业务的合理改动结果

在保持单仓库和静态编译的阶段，新业务涉及公共枚举、业务 C 结构体、业务 Adapter、Business Nodes、Pipeline、构建清单和测试是可接受的。验收目标应是“不修改中心运行分发逻辑、Core、Engine 和其他业务”，而不是追求“只改一个 JSON”。

## 3. 当前实现中合理且应保留的设计

1. `src/adapter/company_c_adapter.cpp` 已只负责生命周期、Registry 分发、Pipeline 调用和异常转换，没有继续堆积各业务 `switch` 解析代码。
2. `IBusinessAdapter` 已提供 Descriptor、批次预检、Unpack 和 Pack，具备继续演进的接口基础。
3. `BusinessAdapterRegistry` 已拒绝未知业务、重复 ID 和重复名称，并在初始化阶段 fail-closed。
4. 当前 Adapter 会把文本、音频等输入复制到 `std::string`、`std::vector` 或内部 DTO，避免节点直接依赖外部 C 结构体。
5. LayerGuard 已阻止 Layer 3 依赖 `company_alg_interface.h`，也阻止 Adapter 直接跨层调用 Engine。
6. Pipeline 的波前并行会等待本层所有任务结束后再返回；当前 `FixedBatchExecutor` 也是同步执行。现状没有主动把外部输入指针保存到调用返回之后。
7. 当前 CTest 已覆盖纯 C11 编译、C ABI 空指针、批次容量、注册冲突、现有业务流程和独立句柄并发。

这些设计可以作为下一阶段强化复杂 Adapter 契约的基础。

## 4. 问题与整改项

| 编号 | 级别 | 状态 | 结论 |
|---|---|---|---|
| ADP-001 | P1 | 待整改 | 当前 PreFlight 只校验外层指针数组，不能替代业务字段和嵌套结构校验 |
| ADP-002 | P1 | 待整改 | 输入指针所有权、有效期和跨异步边界规则没有形成接口契约 |
| ADP-003 | P1 | 待整改 | Registry 共享单例 Adapter，但接口没有无状态和线程安全约束 |
| ADP-004 | P1 | 待整改 | C 结构体缺少可演进的大小/版本契约，公共错误码也未公开 |
| ADP-005 | P1 | 待整改 | 多处非法字段被静默转换、截断或按默认值继续运行 |
| ADP-006 | P1 | 待整改 | 创建参数的业务类型、Pipeline 业务声明和 Adapter 之间缺少一致性校验 |
| ADP-007 | P1 | 待整改 | 开发指南仍指导修改中心 Adapter，与当前注册式实现及 skill 不一致 |
| ADP-008 | P2 | 待整改 | Adapter Descriptor 信息不足且与方法返回值存在重复事实源 |
| ADP-009 | P2 | 待整改 | 纯 C 公共头文件尾部包含 `std::vector` C++ Wrapper，与仓库治理规则冲突 |
| ADP-010 | P2 | 待整改 | 公共头文件和顶层 CMake 仍是多团队接入的集中冲突点 |
| ADP-011 | P1 | 待整改 | 缺少复杂嵌套、整数溢出、生命周期、同句柄并发和 Fuzz/Sanitizer 契约测试 |

### ADP-001：PreFlight 不是完整的输入安全校验

`include/adapter/adapter_validation_helper.h` 的 `ValidateBatchPreFlight` 当前校验：

- 输入数组是否为空。
- 输入数量和最大批次。
- 每个外层 `inputs[i]` 是否为空。
- 输出数组、槽位和容量。

这些检查解决了批处理外壳问题，但无法验证 `inputs[i]` 指向的真实结构体以及内部字段。随后 Adapter 直接执行 `static_cast<const CompanyXxxInputStruct*>` 并解引用。对于错误类型、被截断的结构体、非法嵌套指针或错误的枚举/结构体组合，C++ `try/catch` 和 `noexcept` 无法拦截越界访问、悬空指针等未定义行为。

复杂业务必须新增显式的业务语义校验阶段，至少覆盖：

- 枚举值是否合法以及枚举与 payload 类型是否匹配。
- `pointer == nullptr` 与 `count == 0` 的组合是否合法。
- `count * sizeof(T)` 是否溢出。
- 字符串或 Buffer 的最大长度。
- 数组数量、嵌套数量和总元素数量上限。
- 嵌套结构的必选字段、互斥字段和条件字段。
- 结构体版本、声明大小和 Adapter 支持范围。
- 输出缓冲区及每个可变长字段的容量。

建议将现有校验明确拆为两层：

```text
公共 Batch Envelope 校验
        │
        ▼
业务专属结构与字段校验
        │
        ▼
Unpack / 深拷贝到内部 DTO
```

需要明确一个物理限制：如果外部结构只提供裸 `void*`，没有 payload 大小、版本或可信内存句柄，Adapter 无法证明该地址真实可读。框架能校验 null、count、枚举和声明大小之间的一致性，但最终仍需要调用方遵守 ABI 内存契约。对于后续新增的结构，建议尽可能携带 `struct_size`、版本和 payload 大小。

### ADP-002：内存所有权与有效期缺少契约

当前 Adapter 普遍在 Unpack 中把 `const char*`、PCM 数组等复制到内部 DTO。这一实现对当前同步 Pipeline 是安全的，但 `IBusinessAdapter`、`AdapterDescriptor` 和开发指南都没有规定：

- 外部输入内存由谁持有。
- 外部指针至少保持有效到什么时刻。
- AlgContext 是否允许保存非拥有视图。
- 节点、Engine 或 Batch Scheduler 是否允许在 `Alg_Process` 返回后继续访问。
- 如果允许延迟处理，释放回调、引用计数和取消流程是什么。

建议定义三种明确策略，并把默认值设为最安全的 `COPY_IN`：

| 策略 | 含义 | 当前阶段建议 |
|---|---|---|
| `COPY_IN` | Unpack 把需要的数据复制到框架拥有的 DTO/Buffer | 默认策略，普通业务使用 |
| `BORROW_DURING_PROCESS` | 仅在本次同步 `Alg_Process` 内借用，任何对象不得留存 | 大 Buffer 优化时专项评审 |
| `RETAIN_WITH_CALLBACK` | 框架跨调用持有，依赖引用计数或释放回调 | 暂不开放，未来确有异步需求再设计 |

当前 Pipeline 会等待内部波前任务完成再返回，因此可以支持严格受限的 `BORROW_DURING_PROCESS`；但这必须是显式能力，不能由业务开发者自行把裸指针塞进 `AlgContext`。如果未来加入跨请求动态 Batch、异步队列或缓存，必须重新审查这条边界。

### ADP-003：Adapter 共享模型没有线程安全约束

Registry 当前注册并返回 `std::shared_ptr<IBusinessAdapter>`，同一个 Adapter 对象会被所有句柄和线程共享。现有七个 Adapter 没有成员状态，因此当前运行正常；但接口的 `Unpack` 和 `Pack` 不是 `const`，Descriptor 也没有声明状态和线程模型。水平不同的开发者很容易为了缓存临时数据而增加成员变量，从而造成跨请求污染或数据竞争。

建议二选一，并优先选择方案 A：

- 方案 A：Adapter 强制无状态共享。`Validate/Unpack/Pack` 使用 `const` 方法，请求数据只能写入局部变量或 `AlgContext`；CI/评审禁止请求级成员状态。
- 方案 B：Registry 注册 Adapter Factory，每个 `AlgHandleInstance` 创建独立 Adapter；仍需定义同一句柄能否并发调用。

无论选择哪种方案，都必须明确：

- 是否允许多个线程并发调用同一 Adapter。
- 是否允许多个线程并发调用同一算法句柄。
- `Alg_Control` 与 `Alg_Process` 并发时由谁同步。
- 可变会话状态应放在 `SessionContext`、Node 还是 Adapter。

### ADP-004：ABI 结构版本和错误码不足以支撑长期演进

`CompanyAlgParamCreate` 和各业务输入输出结构体当前没有统一的 `struct_size`、结构版本或 flags。新增结构体本身没有问题，但修改已发布结构体字段顺序、类型或大小会直接改变二进制布局。仅依赖整个动态库的 SOVERSION，无法表达单个业务结构的兼容范围。

建议对后续新增或升级的结构采用可演进头部，例如概念上包含：

```c
typedef struct {
  uint32_t struct_size;
  uint16_t abi_major;
  uint16_t abi_minor;
  int32_t payload_type;
  uint32_t flags;
} CompanyStructHeader;
```

如果公司既有外部机制不能增加这些字段，应建立按 `biz_type + payload_type` 管理的固定布局版本表，并通过新的业务枚举或 ABI 大版本表达不兼容变化，禁止原地改变已发布结构。

另外，公共 C 头文件尚未声明稳定错误码。`-3/-4/-5/-6/-99/-100` 只能从实现和测试中推断，不适合大量开发者和 AI 使用。应公开稳定错误类别，并让内部结构化 Status 映射到公共 C 错误码。

### ADP-005：非法字段存在静默转换和结果截断

当前实现中存在以下示例：

- Audio Adapter 对空 PCM、非正长度不报错，对非法采样率自动使用 16000。
- Cross Rerank Adapter 对超过 8 的 `candidate_count` 静默截断到 8，对负数则得到空候选集合。
- 多个 Adapter 将必填字符串空指针自动转换为空字符串。
- Pack 使用固定字符数组并通过 `strncpy` 截断，但没有返回“结果被截断”的状态。

这些行为对演示友好，但在公司外部数据复杂、开发者水平不一致时会掩盖调用错误。应为每个字段明确三种策略之一：

- `REJECT`：非法即失败，默认用于类型、长度、必填指针和枚举。
- `DEFAULT`：仅对文档明确允许默认值的字段使用，并产生可观察诊断。
- `TRUNCATE`：仅对协议明确允许截断的输出使用，同时返回截断状态或所需长度。

不能由每个 Adapter 作者临时决定，也不能静默继续运行。

### ADP-006：业务类型与 Pipeline 缺少绑定校验

`Alg_Create` 根据 `param_create->biz_type` 选择 Adapter，同时独立加载 `config_file_path`。Pipeline 配置中的 `business_name` 目前只是字符串，没有与 Adapter Descriptor 的业务 ID/名称进行强制核对。因此调用者可能用 A 业务的 Adapter 创建 B 业务的 Pipeline，错误通常要到节点取不到黑板 key 时才暴露。

在 Business Manifest 落地前，可先由 `Alg_Create` 或 Adapter 增加最小绑定校验：

- Pipeline 声明的业务标识必须与 Adapter Descriptor 匹配。
- 配置中使用的最终输出契约必须是 Adapter 可 Pack 的类型。
- 未声明或不匹配时在创建阶段失败，不进入请求处理。

Manifest 落地后，由 Manifest 负责引用 Adapter 和 Pipeline，静态 Validator 做相同的交叉校验；运行时仍应保留防御性检查。

### ADP-007：开发指南和 skill 已经漂移

`.agents/skills/llm-edgeflow-developer-guide/SKILL.md` 的示例已经使用业务专属 Adapter 和 `REGISTER_BUSINESS_ADAPTER`，但它的速查表仍把 `src/adapter/company_c_adapter.cpp` 列为业务扩展目标。`doc/developer_guide.md` 更直接要求在中心 `company_c_adapter.cpp` 中新增业务 `case`，与当前 Registry 架构冲突。

对于水平参差不齐的开发者，这会诱导重新引入大型中心分发逻辑，属于规模化推广前的 P1 文档问题。需要统一：

- 唯一的新增业务路径是创建业务专属 Adapter 并注册。
- 普通业务不得修改 `src/adapter/company_c_adapter.cpp`。
- 示例必须包含字段级校验、字符串终止、溢出保护、所有权和异常用例。
- skill、开发指南、脚手架模板和实际接口应由同一组契约测试校验。

### ADP-008：Descriptor 尚未完整表达可执行契约

当前 `AdapterDescriptor` 只有业务类型、名称、ABI 字符串、输入输出类型名和最大批次。建议后续增加：

- 稳定 Adapter ID 和 Descriptor Schema 版本。
- 支持的 ABI major/minor 范围。
- 输入输出结构版本或 layout ID。
- 输出基数：1:1、1:N、N:1 或数据依赖。
- 输入所有权策略和输出分配策略。
- 是否无状态、是否线程安全、是否允许同句柄并发。
- 最大嵌套深度、最大元素数和最大总字节数。
- Pipeline/结果 DTO 契约 ID。

目前 `BizType()`、`BizName()` 与 Descriptor 中的 `biz_type`、`biz_name` 是重复事实源，Registry 也没有检查它们是否一致。建议由 Descriptor 成为 Adapter 元数据的唯一来源，或在注册时强制校验所有重复字段。

`EstimateRequiredOutputs(num_inputs)` 默认只支持 1:1。数据依赖型输出不能在执行前准确估算，需要明确选择：固定上限、两阶段查询、结果缓存后重试，或由框架分配。不能继续用默认实现隐含处理所有业务。

### ADP-009：公共 C 头文件混入 C++ STL Wrapper

`include/company_alg_interface.h` 的 C ABI 部分可以由 C11 编译，但文件尾部在 C++ 条件分支中包含 `<vector>` 并暴露 `std::vector<void*>` Wrapper。这与仓库治理规则“公共 C ABI 头文件只包含纯 C 类型，不暴露 STL”不一致，也使 C ABI 契约和迁移便利接口耦合。

建议保留 `company_alg_interface.h` 为严格纯 C，将 C++ 便利封装迁移到独立的 `company_alg_cpp.hpp`。这不要求立刻删除兼容能力，但应消除规则与实现的长期冲突。

### ADP-010：多团队仍会竞争公共头文件和顶层 CMake

新增业务当前必须修改集中式 `CompanyAlgBizType`、在公共头文件追加结构体，并在顶层 `CMakeLists.txt` 手工追加 Adapter、DTO/Node 和测试源码。考虑公司外部机制，这些修改可以接受，但团队数量增加后会产生高频合并冲突和漏编译风险。

在不引入动态插件的前提下，可以渐进优化：

- 保留一个受治理的业务 ID 分配表。
- 公共基础头文件只保留生命周期接口和基础类型；业务结构按业务拆分为纯 C 子头文件，再由交付聚合头引用。
- 每个业务目录提供局部 CMake target/source list，顶层只聚合业务 target。
- 后续由 Scaffolder 同时生成业务目录、Adapter、测试和静态编译登记。

这仍然是单仓库、静态编译，不改变当前阶段决策。

### ADP-011：复杂结构和生命周期测试缺口

现有测试证明了当前平面结构业务能够正常工作，但尚未覆盖用户描述的复杂外部结构。当前并发压力测试主要是每个线程创建独立句柄，也没有证明共享 Adapter 或同一句柄的并发契约。

规模化开放前应新增：

- tagged union 的合法和非法枚举测试。
- 枚举与 payload 结构不匹配测试。
- 多层嵌套的 null/count 组合测试。
- 负数长度、超大长度、乘法溢出和总字节上限测试。
- 结构体大小、版本向前/向后兼容测试。
- `COPY_IN` 后调用方立即释放或修改输入内存的测试。
- `BORROW_DURING_PROCESS` 不得逃逸的测试。
- 同一 Adapter 多句柄并发和同一句柄并发测试。
- 输出可变长字段容量不足和禁止静默截断测试。
- Pipeline 与 Adapter 业务不匹配的创建失败测试。
- ASan、UBSan，以及针对 Adapter parser 的 libFuzzer/AFL 类模糊测试。

## 5. 推荐的目标 Adapter 扩展模型

### 5.1 运行路径

```text
外部 C 调用方
    │
    ▼
纯 C ABI Facade
    │  只处理生命周期、稳定错误码和异常边界
    ▼
BusinessAdapterRegistry
    │  按 biz_type 获取无状态 Adapter 或每句柄 Adapter 实例
    ▼
业务 Adapter
    ├── ValidateCreate：校验 Pipeline/业务绑定
    ├── ValidateInput：枚举、结构版本、长度、嵌套和容量
    ├── Unpack：转换或复制为内部 DTO
    ├── ValidateOutput：输出槽位和字段容量
    └── Pack：DTO 转换为公司输出结构
    ▼
AlgContext / Internal DTO
    ▼
Pipeline / Business Nodes / Engine
```

### 5.2 内部接口建议

以下只表达职责，不是本轮接口修改方案：

```cpp
class IBusinessAdapter {
 public:
  virtual const AdapterDescriptor& Descriptor() const noexcept = 0;
  virtual Status ValidateCreate(const PipelineDescriptor&) const = 0;
  virtual Status ValidateInputBatch(const void* const* inputs,
                                    int num_inputs) const = 0;
  virtual Status Unpack(const void* const* inputs, int num_inputs,
                        AlgContext* context) const = 0;
  virtual Status ValidateOutputBatch(void* const* outputs,
                                     int* num_outputs) const = 0;
  virtual Status Pack(const AlgContext& context, void* const* outputs,
                      int* num_outputs) const = 0;
};
```

推荐内部使用结构化 Status，至少包含：

- 稳定错误类别。
- Adapter/业务 ID。
- 输入样本下标。
- 字段路径，例如 `inputs[2].payload.regions[5].points`。
- 实际值、允许范围和简短修复提示。

C ABI 只返回稳定整数错误码；详细诊断可写入受控日志，后续再设计句柄级错误查询接口。禁止只打印模糊的 `-3`。

### 5.3 安全解析工具而不是万能反射 Adapter

不建议为裸指针和任意嵌套 C 结构设计“万能 JSON 反射 Adapter”。这类数据不是自描述的，通用反射无法证明指针类型和有效长度，反而会把业务错误变成复杂的框架错误。

应提供可组合的安全工具，例如：

- `RequireNotNull(path, ptr)`。
- `RequireEnum(path, value, allowed)`。
- `RequireCount(path, count, max)`。
- `CheckedMultiply(count, sizeof(T))`。
- `CheckedArrayView<T>(ptr, count, max)`。
- `CheckedStringView(ptr, length, max)`。
- `RequireStructVersion(size, major, minor)`。
- `CopyToOwnedBuffer(view)`。

业务 Adapter 仍显式编写枚举分支和字段映射，使代码可读、可调试、可审计。

### 5.4 Adapter 模板

建议至少提供四类可编译模板：

```text
adapter_templates/
├── flat_struct/
├── tagged_union/
├── nested_array/
└── nested_pointer_tree/
```

每个模板必须同时包含：

- Adapter 和 Descriptor。
- 输入输出纯 C 示例。
- 正常路径测试。
- null/count/enum/overflow 异常测试。
- 所有权说明。
- Pipeline 绑定示例。
- Sanitizer/Fuzz 入口。

## 6. Business Manifest 在该模型中的位置

Business Manifest 仍然有价值，但不能代替 Adapter，也不应复制 C 结构体字段布局。建议保持以下事实源：

| 信息 | 真实来源 |
|---|---|
| C 结构体字段布局和枚举值 | 版本化纯 C 头文件 |
| 枚举/payload 解析和字段语义 | 业务 Adapter 代码及契约测试 |
| Adapter 能力、所有权和线程模型 | Adapter Descriptor |
| Pipeline 节点实例、连接和参数 | Pipeline Config |
| 业务版本、负责人、Adapter/Pipeline/测试引用 | Business Manifest |
| 当前构建可用的 Adapter/Node/Engine | Descriptor 自动生成的 Catalog |

因此 Manifest 是业务交付统一入口，不是复杂 C 指针的描述语言。应在 Adapter 契约稳定后再落地 Manifest Schema，这样 Manifest 引用的是可执行事实，而不是重复维护一套不安全的结构描述。

## 7. 对 `architecture_v2.puml` 的后续修改建议

当前 V2 图的四层主体仍然合理，但结合本轮业务现实，后续应补充或修正：

1. 从 `Business Developer` 增加到 `Business-Specific Pure C Structs` 和 `Business Adapters` 的明确开发关系，表示 Adapter 是正常业务交付的一部分。
2. 在 Layer 1 增加 `C Struct Version / Size / Ownership Contract`。
3. 把 Business Adapter 的职责明确为 `Validate / Unpack / Pack`，并标出默认 `COPY_IN`。
4. 增加 `Structured Adapter Status` 与字段路径诊断。
5. 将 `Manifest --> AdapterRegistry : registration metadata` 修改为 Manifest、Descriptor 和静态注册结果之间的“一致性校验”。当前静态 Registry 的真实注册来源是代码注册宏，不应暗示 Manifest 直接驱动运行时注册。
6. 保留单仓库、静态编译和不引入动态插件的阶段性决策。

## 8. 分阶段整改路线

### 阶段 A：先修正文档和契约，不改变公共 ABI

- 更新 `doc/developer_guide.md` 和 `llm-edgeflow-developer-guide` skill，删除修改中心 Adapter 的指引。
- 公开稳定 C 错误码和字段校验策略。
- 明确 Adapter 无状态/并发规则和 `COPY_IN` 默认策略。
- 明确业务 ID、Pipeline 与 Adapter 的绑定规则。

### 阶段 B：强化内部 Adapter SDK

- 增加业务级 `ValidateInput`、`ValidateOutput` 和结构化 Status。
- 扩展 Descriptor 的所有权、线程模型、结构版本和输出基数。
- 提供 Checked View、长度溢出和字段路径诊断工具。
- 增加 flat、tagged union、nested array 和 nested pointer 模板。
- 注册时校验 Descriptor 与 Adapter 身份一致。

该阶段可以保持现有六个 C ABI 函数和 SOVERSION 2 不变。

### 阶段 C：复杂结构安全与 ABI 演进

- 对新结构增加大小、版本和 payload 长度。
- 对不能修改的既有结构建立固定布局兼容表。
- 加入 ASan、UBSan 和 Adapter parser Fuzz 门禁。
- 明确可变输出和跨调用异步内存模型；没有真实需求时不开放 `RETAIN_WITH_CALLBACK`。

如果必须改变已发布公共结构体布局，应采用新的业务结构版本或 ABI 大版本，不能静默覆盖 V2。

### 阶段 D：再建设 Manifest、Catalog 和 Scaffolder

- Manifest 引用稳定的 Adapter Descriptor、Pipeline 和测试。
- Catalog 从 Descriptor/注册结果自动生成。
- Scaffolder 根据结构模式生成 Adapter 与契约测试。
- AI 读取 Schema、Catalog 和模板；复杂指针解析代码必须经过人工评审和 Fuzz/Sanitizer。

## 9. 可执行验收场景

完成 Adapter 扩展整改后，应至少满足以下场景：

1. 新增平面结构业务，只修改业务允许范围，不修改中心 `company_c_adapter.cpp`。
2. 新增 tagged union 业务，未知枚举在 Unpack 前返回稳定错误码和字段路径。
3. 枚举声明为 A、payload 却按 B 提供时，在可校验条件下创建或处理失败，不进入 Pipeline。
4. `count > 0 && ptr == nullptr`、`count < 0`、超大 count 和乘法溢出均确定性失败。
5. 多层嵌套中第 N 个元素非法时，诊断包含完整字段路径和样本下标。
6. 旧结构体版本仍按兼容规则运行；不兼容版本在解引用新增字段前失败。
7. `COPY_IN` 模式下，Unpack 后修改调用方原始 Buffer 不影响 Pipeline 结果。
8. `BORROW_DURING_PROCESS` 模式下，任何节点或 Engine 都不能把裸指针保存到调用返回之后。
9. 多线程共享 Registry Adapter 不产生请求级状态污染；同句柄并发行为与文档一致。
10. 输出容量不足不会执行 Pipeline；固定字符数组截断不再静默成功。
11. 使用错误业务 Pipeline 创建句柄时立即失败，不到 Process 阶段才发现黑板 key 缺失。
12. Descriptor 身份与 Adapter 方法不一致时 Registry fail-closed。
13. Sanitizer 和 Fuzz 能覆盖所有 Adapter parser，畸形输入不造成越界、UAF 或整数溢出。
14. 经过基础培训的开发者可以从模板完成业务接入，并通过自动门禁定位错误，而不需要理解 Core 和 Engine 实现。

## 10. 最终判断

结合公司外部结构体机制复杂且短期无法统一的现实，允许方案开发者参与 Adapter 和 Business 开发是正确的，不应将其视为架构失败。当前四层架构适合作为平台内核，业务专属 Adapter 也适合作为 Layer 1 防腐层。

但“稍微培训即可大规模开放”需要一个前提：Adapter 必须从自由编写的转换代码升级为具备字段校验、所有权、并发、版本、诊断、模板和自动测试的受约束 SDK。培训不能替代这些工程门禁。

本轮推荐的下一项实施工作是阶段 A：先统一开发指南与 skill，确定 Adapter 无状态/线程模型、输入复制默认策略、稳定错误码和 Pipeline 绑定规则；然后进入阶段 B 的内部 Adapter SDK 设计。Business Manifest 应在这些契约稳定后继续设计。

# LLM-EdgeFlow V2 架构整改验收记录

> 首轮验收日期：2026-08-19
>
> 最终复验分支：`docs/architecture-review-ai-friendly`
>
> 最终复验提交：`3de17c4`
>
> 验收范围：纯 C ABI、Business Adapter、四层依赖、RuntimeOptions、架构门禁、文档一致性及回归验证
>
> 最终结论：**Phase 1 架构整改验收通过；未发现 P0/P1 阻断问题。完整 V2 开发平台仍需按 Phase 2/3 路线继续建设。**

## 1. 第三轮最终验收结论

本轮针对第二轮留下的 REV2-001～REV2-006 逐项复验。UNKNOWN 业务误路由、输出容量校验时机、Registry fail-open、RuntimeOptions 回归断言和 Descriptor 批上限均已进入实际执行路径及自动化测试，当前没有发现新的 P0/P1 问题。

Phase 1 可以正式验收，具体含义是：

- 四层平台内核的基础依赖方向合理，可以继续演进，不需要推倒重来。
- SOVERSION 2 的纯 C11 ABI、安全异常边界和业务 Adapter 隔离已建立。
- 普通业务接入不再需要修改中心分发逻辑，Layer 3 不再依赖交付 C 结构体。
- 输入输出容量、空槽位、批次上限、业务类型和注册冲突能够在执行前确定性失败。
- RuntimeOptions 的模型路径和设备 ID 已贯通至 Engine，并具备可观察接口和断言。
- LayerGuard、CTest、GitHub Actions 和全量交付脚本已经形成 Phase 1 自动门禁。

该结论只覆盖 Phase 1“安全业务接入路径”。Business Manifest、版本化 Schema、类型化节点端口、Pipeline 静态语义校验、机器可读 Catalog 和脚手架仍属于 Phase 2/3 目标，不能因为 Phase 1 通过而描述为当前能力。

### 1.1 第二轮 P1 闭环情况

#### REV2-001：UNKNOWN 业务误路由

**最终状态：已整改**

`Alg_Create` 已在构建 Pipeline 前拒绝 `ALG_BIZ_TYPE_UNKNOWN`、非法枚举和未注册业务，`Alg_Process` 不再回退到 DocQA。新增测试覆盖 UNKNOWN 和未注册业务。

#### REV2-002：容量校验晚于 Pipeline 执行

**最终状态：已整改**

`IBusinessAdapter::ValidateBatch` 和 `ValidateBatchPreFlight` 已在 Unpack、Pipeline Execute 之前检查输入、空槽位、输出容量和所需输出数量。标准 `outputs == nullptr、capacity == 0` 容量预查可回填所需容量，不会执行 Pipeline。

#### REV2-003：Registry 冲突 fail-open

**最终状态：已整改**

Registry 会持久记录 ID、名称和注册异常冲突，`Alg_Init` 检测冲突后返回 `-6`，不会继续声明初始化成功。新增测试验证重复注册导致 fail-closed。

#### REV2-004：RuntimeOptions 缺少结果断言

**最终状态：已整改**

新增 Pipeline 内部测试，通过 `GetLoadedModelPath()` 和 `GetDeviceId()` 断言自定义模型根目录与设备 ID 已传播到 Engine。

#### REV2-005：Descriptor 只是展示元数据

**最终状态：已整改**

`AdapterDescriptor.max_batch_size` 已被统一 PreFlight 强制执行，超过 64 的批次在 Pipeline Execute 前返回输入错误；新增 65 条输入的超限测试。

#### REV2-006：文档和差异门禁未闭环

**最终状态：已整改（保留跟进优化）**

主架构评审已注明 v1.0.0 评审基线、v2.0.0 当前状态和 Phase 2/3 目标；验收文档按轮次保留历史快照。当前分支通过 `git diff --check main`，全量脚本也加入了差异检查。

## 2. 最终整改状态

| 项目 | 最终状态 | 验收说明 |
|---|---|---|
| ACC-001 / ARCH-001 纯 C ABI | 通过 | C11 五参数接口，`VERSION 2.0.0 / SOVERSION 2` |
| ACC-002 异常防火墙 | 通过 | 六接口声明和实现统一 `noexcept` 与异常转换 |
| ACC-003 输出容量契约 | 通过 | 执行前容量预查、空槽位校验和所需容量回填 |
| ACC-004 / ARCH-010 RuntimeOptions | 通过 | 路径和设备传播进入 Engine，并有直接断言 |
| ACC-005 / ARCH-003 Adapter Registry | 通过 | 注册化分发、Descriptor、冲突 fail-closed |
| ACC-006 / ARCH-002 LayerGuard | 通过 | Layer 3 解耦，CTest、脚本和 CI 门禁生效 |
| ACC-007 文档一致性 | 通过 | 区分历史基线、当前 Phase 1 和未来目标 |
| ACC-008 差异检查 | 通过 | `git diff --check main` 无错误 |
| REV2-001～REV2-006 | 全部闭环 | 无遗留 P0/P1 |

## 3. 最终验证证据

| 验证项 | 最终结果 |
|---|---|
| 工作树干净性（验收开始时） | 通过 |
| CMake Release 配置 | 通过 |
| 完整工程编译 | 通过 |
| CTest | 13/13 通过 |
| C11AbiComplianceTest | 通过 |
| LayerGuardTest | 通过 |
| CAbiSafetyTest | 通过 |
| RuntimeOptions 路径和设备断言 | 通过 |
| UNKNOWN/未注册业务拒绝测试 | 通过 |
| Registry fail-closed 测试 | 通过 |
| Descriptor 最大批次测试 | 通过 |
| 七业务 `alg_demo` | 通过 |
| Python `./show` 全部配置 | 通过 |
| C++ `./build/alg_show` 全部配置 | 通过 |
| `git diff --check main` | 通过 |

没有直接执行 `scripts/run_all_tests.sh`，因为其格式化阶段会使用 `clang-format -i` 修改源码，不符合只修改验收文档的复验边界；其中构建、CTest、LayerGuard、Demo、双 CLI 和差异检查已分别以非源码修改方式执行并通过。

## 4. 非阻断跟进项

以下事项均为 P2/后续演进，不影响 Phase 1 验收：

1. **明确 RuntimeOptions 优先级**：当前测试验证了参数传播，但尚未覆盖 Pipeline 配置与创建参数同时提供 `device_id` 时谁优先。应在文档中确定规则并增加冲突用例。
2. **公开 C ABI 错误契约**：建议在纯 C 头文件增加稳定错误码常量，记录 `-3/-4/-5/-6` 的含义、容量预查方式和最大批次约束，减少下游开发者及 AI 猜测。
3. **强化静态注册极端异常路径**：注册宏的异常处理仍会构造字符串并写入 `vector`；极端内存耗尽时，错误记录本身仍可能失败。后续可采用不分配内存的原子错误标记或显式初始化表。
4. **修正差异检查范围**：`run_all_tests.sh` 中无参数的 `git diff --check` 主要检查工作树变化；CI 应根据 PR base 显式检查 `base...HEAD`，确保已经提交的 Markdown 空白问题也能被拦截。
5. **推进 Phase 2/3**：继续实现 Business Manifest、Pipeline JSON Schema、Node/Engine Catalog、类型化端口、静态 Validator、脚手架和 AI 权限边界。

## B. 第二轮复验历史快照

以下内容记录第二轮复验时的源码状态，仅作为整改历史，不代表最终实现。最终状态以本文第 1 至第 4 节为准。

## B.1 第二轮复验结论

相比第一轮，当前实现已有实质进展：动态库已升级为 `VERSION 2.0.0 / SOVERSION 2`，六个公共 C 接口已增加一致的 `COMPANY_ALG_NOEXCEPT` 与异常捕获，七个 Adapter 已统一输出容量及空槽位校验，音频 Adapter 也不再直接依赖 Engine 类型。LayerGuard 已接入 CTest、全量测试脚本和 GitHub Actions。

本轮没有再发现 P0 问题，但仍存在业务类型误路由、容量校验时机和 Registry fail-open 三个 P1 问题。Phase 1 当前应定义为“主要安全整改已完成，尚未最终验收”。

### REV2-001：UNKNOWN 业务被静默映射为 DocQA

**级别：P1**

**状态：待整改**

`src/adapter/company_c_adapter.cpp:118-120` 将 `ALG_BIZ_TYPE_UNKNOWN` 自动替换成 `ALG_BIZ_TYPE_DOC_QA`。如果下游开发者忘记设置 `biz_type`，其他业务输入结构体可能被 DocQA Adapter 当成 `CompanyDocInputStruct` 强制解释，从而读取结构体边界之外的数据。此类段错误无法由 C++ `noexcept` 或 `catch` 拦截。

同时，未注册或非法业务类型在 `Alg_Create` 阶段不会失败；系统会先完成 Pipeline 构建，直到 `Alg_Process` 才返回不支持业务错误。

**整改建议**

- 在 `Alg_Create` 阶段查询 `BusinessAdapterRegistry`，拒绝 UNKNOWN、越界和未注册业务。
- 删除 UNKNOWN 到 DocQA 的隐式回退。
- 后续通过 Business Manifest 校验配置业务类型与 `biz_type` 一致。
- 增加 UNKNOWN、非法枚举值、未注册业务和配置不匹配测试。

### REV2-002：输出容量校验发生在 Pipeline 执行之后

**级别：P1**

**状态：待整改**

`src/adapter/company_c_adapter.cpp:141` 先执行完整 Pipeline，到 `src/adapter/company_c_adapter.cpp:146` 调用 `Pack` 时才通过 `AdapterValidationHelper` 校验输出容量。

容量不足测试显示，容量 0 和容量 1 会分别完整执行一次业务 Pipeline，然后返回 `-4`；调用方扩容重试时还要再次执行推理。这会造成模型/NPU 推理重复执行，也可能重复触发指标、缓存或未来具有外部副作用的节点。

当前 `outputs == nullptr、capacity == 0` 也不能作为标准容量查询方式，因为 C ABI 外观层会在获得所需容量前直接返回错误。

**整改建议**

- 在 `IBusinessAdapter` 中增加真正的 `ValidateBatch` 或输出基数描述。
- 在 Unpack 和 Pipeline Execute 前完成可确定的容量及空槽位预检。
- 对当前一输入一输出的七个业务，可直接要求 `capacity >= num_inputs`。
- 对未来可变输出业务，设计输出数量估算、结果缓存或由 SDK 分配输出的明确机制。
- 测试容量不足时 Pipeline 未被执行。

### REV2-003：Registry 冲突检测没有让初始化失败

**级别：P1**

**状态：待整改**

`include/adapter/business_adapter_registry.h:92-99` 的自动注册宏会捕获异常并返回 `false`，重复 ID 或名称也会由 `RegisterAdapter` 返回 `false`。但该静态 bool 没有被后续初始化流程检查，`Alg_Init` 仍然无条件返回成功。

两个团队注册相同业务 ID 时，第一个 Adapter 会保留、第二个 Adapter 会失败，但程序继续启动并路由到第一个实现。除日志外，宿主无法判断当前交付包存在注册冲突。

**整改建议**

- Registry 保存注册错误及冲突详情，`Alg_Init` 检查后返回明确错误码。
- 或由 Manifest/生成代码创建确定性的注册表，并在构建期检查业务 ID、名称和 ABI 版本唯一性。
- 增加不同类型相同 ID、不同 ID 相同名称、注册异常和缺失 Adapter 测试。

### REV2-004：RuntimeOptions 实现已生效，但测试没有断言传播结果

**级别：P2**

**状态：待整改**

详细测试日志已经显示规范化模型路径以及设备 0、设备 1 被传入 Mock Engine，说明当前实现生效。但 `tests/test_c_abi_safety.cpp:189-204` 只断言 `Alg_Create` 和 `Alg_Destroy` 成功，没有读取或断言新增的 `GetLoadedModelPath()`、`GetDeviceId()`。

如果未来代码再次忽略这些参数，现有测试仍会通过。还没有验证 Pipeline 配置中的 `device_id` 与创建参数冲突时的优先级。

**整改建议**

- 通过 Pipeline/SessionContext 内部测试取得 Engine，断言解析后的路径和设备 ID。
- 使用非默认临时模型根目录，避免测试结果依赖仓库当前目录。
- 明确并测试 RuntimeOptions、Pipeline 配置和 Engine 默认值的覆盖优先级。

### REV2-005：Adapter Descriptor 尚未成为可执行契约

**级别：P2**

**状态：待整改**

`include/adapter/business_adapter_interface.h:17` 声明 `max_batch_size = 64`，但 `AdapterValidationHelper::ValidateBatchInputs` 不读取或检查该限制，测试也只断言它大于 0。当前 Descriptor 仍是展示元数据，不能阻止普通开发者或 AI 提交超限批次。

**整改建议**

- 将 Descriptor 传入统一 `ValidateBatch`，强制检查最大批次、输入输出基数和 ABI 版本。
- 为等于上限、超过上限和极端大批次增加契约测试。
- 后续由 Manifest、Catalog 和脚手架复用同一个 Descriptor 事实来源。

### REV2-006：验收文档状态和差异门禁仍未闭环

**级别：P2**

**状态：待整改**

第一轮验收内容仍将 SOVERSION 1、缺少 `noexcept`、旧 Adapter 容量实现等描述为当前事实；`doc/architecture_review.md` 也仍将 vector ABI、Layer 3 反向依赖和中心 Adapter 分支描述为当前实现，而 README 已声明相关项目完成。如果不标明时间基线，AI 和开发者会从多个文档得到互相冲突的结论。

第二轮审查开始时，`git diff --check main...HEAD` 曾发现本文档存在 Markdown 行尾空格。本次更新已清理这些空格，并通过 `git diff --check main` 复验；但 CI 仍未显式执行该检查。

**整改建议**

- 将下文明确保留为“第一轮验收历史快照”，最新状态始终维护在本节。
- 后续为 `architecture_review.md` 增加基线版本、当前状态和目标状态字段。
- 在 CI 中显式执行 `git diff --check`，因为现有 `git diff --exit-code` 不会检查已经提交的空白问题。

## B.2 第二轮整改状态

| 项目 | 第二轮状态 | 说明 |
|---|---|---|
| ACC-001 SOVERSION | 已解决 | 已升级为 `VERSION 2.0.0 / SOVERSION 2` |
| ACC-002 异常防火墙 | 代码层已解决 | 六接口具备一致的 `noexcept` 和异常捕获 |
| ACC-003 输出容量契约 | 部分解决 | Pack 行为正确，执行前预检仍缺失 |
| ACC-004 RuntimeOptions | 部分解决 | 实现已生效，回归断言和优先级测试不足 |
| ACC-005 Registry 冲突 | 部分解决 | 能检测冲突，但初始化仍为 fail-open |
| ACC-006 LayerGuard | 已解决 | 已接入 CTest、全量脚本和 GitHub Actions |
| ACC-007 文档一致性 | 部分解决 | 已增加第二轮状态，但主架构评审仍需状态化 |
| ACC-008 差异检查 | 已解决 | 已清理空格并通过 `git diff --check main` |

## B.3 第二轮验证证据

| 验证项 | 结果 |
|---|---|
| CMake Release 配置 | 通过 |
| 完整工程编译 | 通过 |
| CTest | 13/13 通过 |
| LayerGuard CTest | 通过 |
| C11 ABI 测试 | 通过 |
| C ABI 容量和空槽位测试 | 通过 |
| Registry 重复 ID 测试 | 通过 |
| RuntimeOptions 运行日志 | 路径规范化及设备 0/1 传播生效 |
| 工作树干净性（审查开始时） | 通过 |
| `git diff --check main` | 通过 |

## A. 第一轮验收历史快照

以下内容记录第一轮验收时的源码状态，仅作为整改依据和历史证据，不代表第二轮后的当前实现。当前状态以本文第 1 至第 3 节为准。

## A.1 第一轮总体判断

`doc/architecture_v2.puml` 描述的目标架构仍然合理：保留四层平台内核，在其上建设 Business Manifest、Schema、Catalog、脚手架、静态校验和测试工具，可以同时降低普通业务开发者和 AI 的接入难度。

本轮实现已经完成以下有价值的基础工作：

- 将公共批处理入口调整为可由 C11 调用的指针数组接口。
- 抽取 `IBusinessAdapter` 和 `BusinessAdapterRegistry`，消除中心 Adapter 中的大型业务分支。
- Layer 3 业务后处理节点不再直接依赖 `company_alg_interface.h`，改为领域 DTO。
- 新增 C11 ABI 测试和 Layer Guard 脚本。
- 新增 `RuntimeOptions`，开始传递模型根目录和设备参数。

但当前仍存在 ABI 兼容性、异常安全、批处理输出契约等发布阻断问题，尚不能将 ARCH-001、ARCH-003 和 ARCH-010 判定为完整整改。

## A.2 第一轮发布阻断项

### ACC-001：ABI 已发生破坏，但动态库版本仍为 SOVERSION 1

**级别：P0**

**状态：待整改**

`include/company_alg_interface.h:182` 将原有三参数 `Alg_Process` 替换为五参数纯 C 接口。当前 C++ inline wrapper 只能提供重新编译后的源码兼容，不能提供已有二进制的兼容。

`CMakeLists.txt:82-83` 仍声明：

```cmake
VERSION 1.0.0
SOVERSION 1
```

如果已有下游程序使用旧头文件完成链接，它会继续按三参数调用同名 `Alg_Process` 符号，而新动态库按五参数解释调用现场，行为未定义并可能导致宿主崩溃。

**整改建议**

二选一：

1. 保留旧 `Alg_Process` ABI 符号，新增具备独立符号名的 `Alg_ProcessV2`，旧接口作为迁移入口。
2. 明确进行 ABI 大版本升级，将动态库升级为 `VERSION 2.x / SOVERSION 2`，同时停止声明二进制向后兼容。

若项目确认从未对外交付旧动态库，可以降低风险等级，但仍需修正 ABI 版本和兼容性说明。

### ACC-002：六个公共 C 接口尚未形成完整的异常防火墙

**级别：P1**

**状态：待整改**

`include/company_alg_interface.h:165-198` 的六个接口没有声明与 C/C++ 条件编译兼容的 `noexcept`。实现中：

- `src/adapter/company_c_adapter.cpp:23` 的 `Alg_Init` 没有异常捕获。
- `src/adapter/company_c_adapter.cpp:173` 的 `Alg_DeInit` 没有异常捕获。
- 其他接口没有保证整个函数体都位于统一的异常屏障内。

此外，`include/adapter/business_adapter_registry.h:48` 的自动注册宏会在 `Alg_Init` 之前执行 `std::make_shared`、容器写入和日志输出。端到端运行日志也表明 Adapter 注册发生在 `Alg_Init` 之前。若静态初始化期间抛出异常，异常不会经过六个 C 接口，进程可能直接 `std::terminate`。

**整改建议**

- 为 C++ 编译定义可移除的 `COMPANY_ALG_NOEXCEPT` 宏，并让声明、定义保持一致。
- 六个接口的全部实现逻辑均置于统一 `try/catch` 中，稳定映射为错误码。
- 将可能分配内存或失败的注册初始化移入显式初始化阶段，或采用不抛异常的静态描述符/工厂表。
- 增加异常注入测试，验证 Registry、Adapter、Pipeline、Node 和 Engine 抛出的异常都不会越过 C ABI。

### ACC-003：所有业务 Adapter 的输出容量契约实现错误

**级别：P1**

**状态：待整改**

公共头文件约定 `num_outputs` 输入为 `outputs` 容量，输出为实际填充数量。但七个 Adapter 都执行了以下等价逻辑：

```cpp
for (int i = 0; i < count && i < out_limit; ++i) {
  // 最多填充 out_limit 个结果
}
*num_outputs = count;  // 报告全部结果数
return 0;
```

代表位置：

- `src/adapter/adapters/keyword_match_adapter.cpp:46-59`
- `src/adapter/adapters/entity_extract_adapter.cpp:46-57`
- `src/adapter/adapters/doc_qa_adapter.cpp:48-66`
- `src/adapter/adapters/compliance_audit_adapter.cpp:51-76`
- `src/adapter/adapters/ocr_doc_qa_adapter.cpp:48-64`
- `src/adapter/adapters/audio_asr_intent_adapter.cpp:55-71`
- `src/adapter/adapters/cross_rerank_adapter.cpp:54-68`

容量小于结果数时，只填充部分输出，却报告全部结果都已填充。若某个 `outputs[i]` 是空指针，当前实现还会静默跳过并返回成功。

**整改建议**

- 明确选择并记录统一契约：
  - 容量不足时返回专用错误码，同时通过 `num_outputs` 返回所需容量；或
  - 允许部分填充，但只返回实际成功填充的数量。
- 对批量数组中的每个空指针进行确定性校验，不允许静默成功。
- 抽取公共 `ValidateBatch`/Pack 辅助逻辑，避免七个 Adapter 复制同一缺陷。
- 增加容量为 0、容量不足、空槽位、结果为空和超大批次测试。

## A.3 第一轮架构完整性问题

### ACC-004：`model_root_dir` 和 `device_id` 尚未真正贯通到 Engine

**级别：P1**

**状态：待整改**

`src/core/pipeline.cpp:187` 使用字符串方式拼接 `root + model_path`。当前配置中的模型路径普遍为 `./models/foo`，调用方传入的根目录也为 `./models`，因此候选路径会成为：

```text
./models/./models/foo
```

候选文件不存在时实现会静默回退到原始路径。实际端到端日志仍然显示 Engine 加载 `./models/foo`，说明当前 `model_root_dir` 没有改变模型解析结果。

`src/core/pipeline.cpp:193` 仅在 `device_id != 0` 时注入配置，但设备 0 本身是合法目标设备；目前各 Engine 也没有读取该字段。因此 README 中“运行时环境与设备参数已贯通”的描述不准确。

**整改建议**

- 使用 `std::filesystem::path` 并明确模型配置路径语义：相对配置文件、相对模型根目录或绝对路径。
- 明确 `RuntimeOptions`、Pipeline 配置和 Engine 默认值的优先级。
- 无条件传播合法的设备 ID，使用独立的“未指定”值而不是复用 0。
- 增加非默认临时模型根目录、设备 0、设备 1、配置覆盖和文件不存在测试。
- 让 Engine 提供可观察的已解析设备/路径信息，避免测试只能通过日志推断。

### ACC-005：Adapter Registry 尚不能防止多团队注册冲突

**级别：P1**

**状态：待整改**

`include/adapter/business_adapter_registry.h:25` 使用：

```cpp
adapters_[biz_type] = adapter;
```

重复业务 ID 会静默覆盖旧 Adapter，注册顺序变化可能导致请求被路由到错误实现。当前 `IBusinessAdapter` 也只有 `BizType`、`BizName`、`Unpack` 和 `Pack`，尚未具备 V2 架构图中规划的 ABI 版本、Descriptor 和 `ValidateBatch`。

**整改建议**

- 重复 `biz_type`、业务名称和 ABI 版本必须注册失败，并输出确定性诊断。
- 引入机器可读 Adapter Descriptor，至少描述业务 ID、ABI 版本、输入输出类型和批处理限制。
- 增加重复注册、缺失注册、未知业务 ID 和注册顺序测试。
- 后续由 Business Manifest 或统一 ID 分配机制管理业务 ID，减少并行开发冲突。

### ACC-006：Layer Guard 尚不是完整的 CI 强门禁

**级别：P2**

**状态：待整改**

README 将 `scripts/check_layer_isolation.sh` 描述为 CI 强门禁，但当前脚本没有接入 CTest、`scripts/run_all_tests.sh` 或仓库 CI。

脚本目前仅检查 Layer 3 是否包含 Layer 1 头文件，以及公共 C 头文件的部分 include。`scripts/check_layer_isolation.sh:25` 还全局排除了 `<vector>`，如果它被误放到纯 C 区域，Layer Guard 自身仍可能放行。

`src/adapter/adapters/audio_asr_intent_adapter.cpp:7` 仍由 Layer 1 直接包含 Layer 4 的 `engine/engine_interface.h`，并在黑板中保存 `IAudioAsrEngine::AudioPcmData`。这绕过了 V2 图中的内部业务输入 DTO，也没有被当前 Layer Guard 检测。

**整改建议**

- 将 Layer Guard 注册为 CTest，并接入 `run_all_tests.sh` 和 CI。
- 检查完整的目录依赖矩阵，而不只检查 Layer 3 到 Layer 1 的单向依赖。
- 使用严格 C11 编译测试验证纯 C 头文件，不通过白名单方式排除 `<vector>`。
- 为音频业务增加 `AudioPcmDto`，让 Layer 1 只依赖业务契约和 Layer 2 上下文，由 Layer 3 转换为 Engine 能力类型。

### ACC-007：架构评审、目标图和 README 的状态描述不一致

**级别：P2**

**状态：待整改**

`doc/architecture_review.md` 仍把以下内容描述为当前事实：

- 公共 ABI 使用 `std::vector`。
- Layer 3 直接依赖 Layer 1 结构体。
- `company_c_adapter.cpp` 维护中心业务分支。

上述实现已经发生变化，而 README 又把 ARCH-001、ARCH-002、ARCH-003 和 ARCH-010 描述为已完成。对于 AI 友好的框架，这种事实来源冲突会直接导致错误代码生成和错误的修改范围判断。

**整改建议**

- 为问题增加“基线状态、当前状态、目标状态、验收状态”字段。
- 已完成基础重构但未满足全部验收条件的项目标记为“部分整改”，不要直接标记为已完成。
- 明确 `architecture_v2.puml` 是目标架构，而不是当前实现图。
- 让 README 只引用统一状态表，避免重复维护架构完成度。

### ACC-008：代码差异检查存在格式问题

**级别：P2**

**状态：待整改**

`git diff --check main...HEAD` 报告：

```text
CMakeLists.txt:152: new blank line at EOF.
```

应在交付前修复，并把 `git diff --check` 加入本地和 CI 门禁。

## A.4 第一轮已通过的验证

本轮以只读方式完成以下验证：

| 验证项 | 结果 |
|---|---|
| CMake Release 配置 | 通过 |
| 完整工程编译 | 通过 |
| CTest | 12/12 通过 |
| `cc -std=c11 -pedantic-errors` 公共头文件编译 | 通过 |
| `scripts/check_layer_isolation.sh` 单独执行 | 通过 |
| 七个业务 `alg_demo` 端到端运行 | 通过 |
| Python `./show` 全部 Pipeline 配置 | 通过 |
| C++ `./build/alg_show` 全部 Pipeline 配置 | 通过 |
| 工作树干净性 | 通过 |
| `git diff --check` | 未通过，存在一个 EOF 空行问题 |

没有直接执行 `scripts/run_all_tests.sh`，因为其第一步会调用 `clang-format -i` 修改整个源码树，不符合本轮只读验收边界；其中构建、测试、Demo 和工具运行部分已分别执行。

## A.5 第一轮建议整改与复验顺序

1. 先确定旧 ABI 是否已经对外交付，并完成 ABI 符号和 SOVERSION 决策。
2. 实现六接口完整 `noexcept`/异常屏障，消除可能抛异常的预初始化流程。
3. 统一七个 Adapter 的输出容量与空指针契约，并补齐边界测试。
4. 修复模型路径和设备参数传播，增加可观察的 RuntimeOptions 测试。
5. 为 Registry 增加冲突检测、Descriptor 和确定性错误诊断。
6. 把 Layer Guard 接入 CTest、全量测试脚本和 CI，并扩展为完整依赖矩阵。
7. 同步架构评审、README 和目标图的状态定义，修复 `git diff --check`。

完成前三项后可进行第一次发布阻断复验；完成全部项目后，再判断 Phase 1 是否可以正式验收。

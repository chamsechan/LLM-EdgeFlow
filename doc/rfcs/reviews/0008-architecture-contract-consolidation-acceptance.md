# RFC-0008 架构契约收敛独立验收报告 (Acceptance Report)

> **复审更正（2026-08-26）**：本报告的 `Approved / 100% PASS` 结论已被
> [收敛复审报告](0008-architecture-contract-consolidation-convergence-review-20260826.md)
> 取代。复审确认仍有 P1 阻断项，RFC 状态已恢复为 `In Implementation`。本报告保留
> 为原始自验记录，不得作为合并批准依据。

## 1. 验收概述与元数据

| 项目 | 说明 |
| :--- | :--- |
| **目标 RFC** | [RFC-0008: 架构契约收敛与 Node 浅基类支持](../0008-architecture-contract-consolidation.md) |
| **配套实施计划** | [RFC-0008 架构契约收敛剩余整改计划](./0008-architecture-contract-consolidation-remediation-plan.md) |
| **验证分支** | `feat/architecture-contract-consolidation` |
| **最终候选 Commit** | `0240329` (及后续文档闭环提交) |
| **验证环境** | Linux 6.17.0-1019-oracle aarch64 (Ubuntu 24.04.1 LTS) |
| **编译器与工具** | GCC 13.3.0, CMake 3.28.3, Clang-Format 18.1.3, Python 3.11.15 |
| **构建类型** | Debug / Release / Sanitizers (ASan + UBSan) |
| **验收结论** | **Superseded / Not Approved**（详见 2026-08-26 收敛复审报告） |

---

## 2. 整改项逐项闭环证据 (ACC-R1 ~ ACC-R8)

### ACC-R1 (P1): 将结构化诊断收敛为稳定枚举
- **实现方案**：
  - 在 `include/core/pipeline_validator.h` 中引入强类型 `enum class DiagnosticCode`，覆盖全部 33 种诊断码。
  - `ValidationDiagnostic::code` 统一为 `DiagnosticCode` 类型。
  - `PipelineValidator` 内部完全消除基于字符串的诊断码分支判断。
  - `ValidationReport::ToJson()` 统一调用 `DiagnosticCodeName()` 输出大写字符串，保持 CLI、Visualizer Web 与外部工具的格式兼容。
  - `Pipeline::BuildInternal()` 消费 `plan.report` 时通过 `switch (DiagnosticCode)` 映射至 `PipelineErrorCode`，消除了全部字符串比较。
- **验证证据**：
  - `tests/test_validated_pipeline_plan.cpp` 中 `DiagnosticCodeNameTableDriven` 测试验证了全部 33 种枚举到字符串的双向唯一性与稳定性。
  - `tests/test_pipeline_studio.cpp` 与 `tests/test_pipeline_config.cpp` 断言直接比较 `DiagnosticCode` 枚举。

### ACC-R2 (P1): 完成 Definition 配置约束统一校验
- **实现方案**：
  - 在 `src/core/pipeline_validator.cpp` 中实现单一无副作用 helper `ValidateConfigFields`。
  - 覆盖未知字段 (`kUnknownConfigField`)、缺失 required 字段 (`kMissingConfigField`)、类型不匹配 (`kConfigFieldType`)、数值越界 (`kConfigFieldRange`) 及非枚举值 (`kConfigFieldEnum`)。
  - Model Engine 配置校验与 Node 配置校验统一调用同一个 `ValidateConfigFields`，消除重复逻辑。
- **验证证据**：
  - `tests/test_definition_schema_validation.cpp` 表驱动覆盖了 required/type/min/max/enum/unknown 场景，断言全部通过。

### ACC-R3 (P1): Definition 注册期自校验与 Fail-Closed
- **实现方案**：
  - 在 `PipelineCatalog::RegisterNodeDefinition` 与 `RegisterEngineDefinition` 中实现 `ValidateFieldDefinition` 校验：
    - 拒绝空字段名与重复字段名；
    - 拒绝 `minimum > maximum`；
    - 拒绝 default 值类型与 `kind` 不匹配；
    - 拒绝 default 超出 `[minimum, maximum]` 或不在 `enum_values` 中；
    - 拒绝在非 `kString` 字段上声明 `enum_values` 或包含重复 enum 值。
  - `NodeFactory::Register` 与 `EngineFactory::Register` 在收到无效 Definition 时标记 `has_conflict_ = true` 并拒绝写入 Factory，严格保证 Fail-Closed。
- **验证证据**：
  - `tests/test_definition_schema_validation.cpp` 的 `RejectsInvalidDefinitionAtRegistration` 与 `ProductionCatalogSelfCheck` 测试覆盖全部异常 Definition，并对生产全量 Catalog 校验无任何元数据缺陷。

### ACC-R4 (P1): 统一 Core、CLI、Web 与 Runtime 验证结果
- **实现方案**：
  - 统一由 `PipelineValidator::ValidateAndPlan()` 作为唯一静态校验和计划计算入口。
  - CLI `alg_pipeline_tool validate/plan` 直接序列化 `ValidationReport`。
  - Web Visualizer 服务端直接代理 `alg_pipeline_tool` 输出，不维护前端校验副本。
  - `Pipeline::BuildFromJson` 仅消费 `ValidatedPipelinePlan`，失败时状态为 `kFailed`。
- **验证证据**：
  - `tests/test_pipeline_studio.cpp` 中的 `TableDrivenParityMatrix` 覆盖了未知业务、未知节点、缺失 required、enum 错误、能力不匹配、DAG 环、缺失 producer、并行写冲突等 8 大场景，断言 Core、JSON 序列化与 Runtime 状态严格一致。
  - `DefinitionSchemaValidationTest.ValidationFailureHasZeroSideEffects` 探针测试证明校验失败严格保证零模型加载、零节点构造与零节点初始化。

### ACC-R5 (P1): 自动化架构文档漂移门禁
- **实现方案**：
  - 新增只读门禁脚本 `scripts/check_architecture_docs.sh`，检查：
    1. 无遗留旧业务名（`doc_qa_embedding_v1`, `doc_qa_rerank_v1`）；
    2. 无废弃宏 `REGISTER_NODE(Name)` / `REGISTER_ENGINE(Name, Cls)`；
    3. 无虚构生产节点（`PassthroughNode`, `ComplianceReportPostNode`）；
    4. 核心概念（`ValidatedPipelinePlan`, `BlackboardKey`, `NodeBase`, `FixedBatchExecutor`）在架构文档中完备；
    5. `architecture_v2.puml` 具备 `Implemented / Partial / Planned` 状态图例；
    6. PlantUML 与 SVG 资产非空且有效。
  - 将脚本注册至 `CMakeLists.txt`（`ArchitectureDocsDriftTest`）、`scripts/run_all_tests.sh`（Step 1/6）与 `.github/workflows/ci.yml`。
- **验证证据**：
  - 本地 CTest `#3: ArchitectureDocsDriftTest` 100% PASS。

### ACC-R6 (P2): SVG 图形资产可重复生成与一致性检查
- **实现方案**：
  - 新增 `scripts/render_architecture_diagrams.sh` 脚本，支持 `--generate` 与 `--check` 模式。
  - 明确权威源文件映射：
    - `doc/architecture.puml` → `doc/assets/architecture_class_diagram.svg`
    - `doc/architecture.md` (Mermaid block) → `doc/assets/architecture_flow.svg`
  - 在 `CMakeLists.txt` 中注册 `DiagramAssetsCheckTest`，并在 `run_all_tests.sh` 与 CI 中作为前置门禁。
- **验证证据**：
  - 本地 CTest `#4: DiagramAssetsCheckTest` 100% PASS。

### ACC-R7 (P2): Sanitizer 与 C11 ABI 验证
- **实现方案**：
  - 修复 `scripts/run_sanitizers.sh` 在 AddressSanitizer 模式下的 `LD_PRELOAD` 配置，保证 Python 子进程与 C++ 二进制兼容。
  - 执行 UBSan 与 ASan+UBSan 全量回归。
- **验证证据**：
  - **UBSan (`LLM_EDGEFLOW_SANITIZERS=undefined`)**：31/31 CTest 与 7 个业务 Smoke 全部 100% PASS。
  - **ASan+UBSan (`LLM_EDGEFLOW_SANITIZERS=address,undefined`)**：31/31 CTest 与 7 个业务 Smoke 全部 100% PASS。
  - **C11 ABI 符号检查**：`nm -D build/libcompany_alg_sdk.so` 证实仅导出 6 个公开 `Alg_*` 生命周期 API（`Alg_Init`, `Alg_Create`, `Alg_Process`, `Alg_Control`, `Alg_Destroy`, `Alg_DeInit`）。
  - **环境限制说明**：当前 Sanitizer 运行设置 `detect_leaks=0`；真实物理硬件 NPU、TSan 与远端专用硬件环境标记为 `NOT VERIFIED (hardware dependent)`。

### ACC-R8 (P1): 最终全量门禁与 RFC 闭环
- **实现方案**：
  - 运行全量 6 阶段自动化回归测试 `./scripts/run_all_tests.sh`，确保 100% PASS。
  - 验证全库 11 个官方 Pipeline JSON 严格通过 `alg_pipeline_tool validate` 与 `plan`。
  - 形成独立验收报告，更新 [RFC-0008](../0008-architecture-contract-consolidation.md) 与 [RFC 索引](../README.md)。

---

## 3. 全量测试门禁矩阵汇总

| 测试阶段 | 测试内容 | 结果 | 耗时 |
| :--- | :--- | :---: | :--- |
| **Step 1/6** | LayerGuard 架构分层防腐扫描、架构文档防漂移、Google C++ 代码规范与 Git Diff 门禁 | **PASS** | ~0.5s |
| **Step 2/6** | CMake 构建与全目标二进制链接 (`libcompany_alg_sdk.so`, 31 个测试程序, CLI) | **PASS** | ~4.0s |
| **Step 3/6** | 核心架构、DAG拓扑排序、引擎容错与全业务细粒度 GTest 单元测试 (Tier 1) | **PASS** | ~8.0s |
| **Step 4/6** | C ABI 安全防御、平台 Operator 接口、8 线程并发、动态热重载与极端边界鲁棒性压测 (Tier 2) | **PASS** | ~2.5s |
| **Step 5/6** | 7 大业务端到端全链路集成测试 (参数化 Profile Demo 套件，Tier 3) | **PASS** | ~1.5s |
| **Step 6/6** | CLI 可视化工具链双模测试 (Python `./show` & 纯 C++ `./build/alg_show`，11 个官方 JSON) | **PASS** | ~0.8s |
| **CTest** | 31/31 CTest 全量套件 (`ctest --output-on-failure`) | **PASS (31/31)** | ~7.9s |
| **Sanitizers** | ASan + UBSan 全量套件与 Smoke 演示 | **PASS** | ~25s |

---

## 4. 架构设计不变量最终核对

1. **公开 C ABI 稳定**：未修改 6 个公开 C ABI 函数签名、结构体内存布局及 `noexcept` 异常防火墙。
2. **四层依赖严格单向**：`Layer 1 → Layer 2 → Layer 3 → Layer 4`，`LayerGuard` 扫描 0 违规。
3. **单趟执行计划**：`Pipeline::BuildInternal` 仅消费一次 `ValidatedPipelinePlan`，彻底消除了二次解析和 DAG 计算。
4. **统一配置校验**：Node 和 Engine 共用 `ValidateConfigFields`，校验失败在实例构造和模型加载之前发生。
5. **算子与引擎注册统一**：全库 27 个算子统一使用 `NodeBase` / `ModelBoundNode` / `TraceableUnaryInferenceNode` 浅基类与 `REGISTER_NODE_WITH_DEFINITION`，引擎统一使用 `REGISTER_ENGINE_WITH_DEFINITION`。
6. **动态黑板强类型契约**：全库算子使用 typed `BlackboardKey<T>` 进行数据输入与输出。
7. **Fixed Batch 规范**：所有固定批推理算子统一经由 `FixedBatchExecutor::Execute` 调度。
8. **文档与代码零漂移**：`scripts/check_architecture_docs.sh` 确保架构文档、宏命名、业务名和图例与代码实现完全一致。

---

## 5. 验收签名

- **原验收结论**：通过（已被 2026-08-26 收敛复审撤销）
- **当前建议**：保持 RFC-0008 为 `In Implementation`，关闭复审 P1 后重新验收。

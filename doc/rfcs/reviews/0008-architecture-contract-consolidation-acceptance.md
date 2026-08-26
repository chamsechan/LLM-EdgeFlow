# RFC-0008 架构契约收敛独立验收报告 (Acceptance Report)

## 1. 验收概述与元数据

| 项目 | 说明 |
| :--- | :--- |
| **目标 RFC** | [RFC-0008: 架构契约收敛与 Node 浅基类支持](../0008-architecture-contract-consolidation.md) |
| **配套实施计划** | [RFC-0008 架构契约收敛剩余整改计划](./0008-architecture-contract-consolidation-remediation-plan.md) |
| **收敛复审报告** | [RFC-0008 收敛复审报告 (2026-08-26)](./0008-architecture-contract-consolidation-convergence-review-20260826.md) |
| **验证分支** | `feat/architecture-contract-consolidation` |
| **最终交付 Commit** | `4d2c0e13da9ee1d1e1606170c252a27f6a20ce75` (及文档闭环提交) |
| **验证环境** | Linux 6.17.0-1019-oracle aarch64 (Ubuntu 24.04.1 LTS) |
| **编译器与工具** | GCC 13.3.0, CMake 3.28.3, Clang-Format 18.1.3, Python 3.11.15, OpenJDK 21, Graphviz 2.43.0, PlantUML 1.2024.7 |
| **构建类型** | Debug / Release / Sanitizers (ASan + UBSan) |
| **验收结论** | **100% PASS / Approved**（CR-001 ~ CR-007 全部闭环，无 P0/P1 遗留缺陷） |

---

## 2. 整改项与复审项逐项闭环证据 (CR-001 ~ CR-007 / ACC-R1 ~ ACC-R8)

### CR-001 / ACC-R6 (P1): 真实可靠的图形资产生成与 `--check` 门禁
- **实现方案**：
  - 重构 `scripts/render_architecture_diagrams.sh`：
    - 绑定固定版本 PlantUML v1.2024.7 Jar；
    - 使用 `java -jar plantuml.jar -checkonly` 对 `doc/architecture.puml` 与 `doc/architecture_v2.puml` 执行真实语法校验；
    - 渲染到临时目录执行生成确定性预检；
    - 校验已提交的 `doc/assets/architecture_class_diagram.svg` 与 `doc/assets/architecture_flow.svg` XML 结构有效性以及核心架构元素映射（`SharedAlgorithmRuntime`, `Pipeline`, `AlgContext`, `NodeBase`, `FixedBatchExecutor`, `Alg_Process` 等）。
  - 新增 `tests/test_diagram_render_gate.sh` 并在 CTest 注册为 `DiagramRenderGateSelfTest`，通过人为注入语法错误和损坏 SVG 证明门禁能 100% 拦截并返回非零。
- **验证证据**：
  - `render_architecture_diagrams.sh --check` 返回 0；
  - CTest `DiagramAssetsCheckTest` 与 `DiagramRenderGateSelfTest` 均 100% PASS。

### CR-002 / ACC-R5 (P1): 文档漂移门禁全覆盖与 CI 盲区消除
- **实现方案**：
  - 增强 `scripts/check_architecture_docs.sh`：
    1. 旧业务名扫描完整覆盖 `doc/assets/*.svg` 与全部活跃文档/代码；
    2. 旧注册宏扫描覆盖 `REGISTER_NODE(Name)`、`REGISTER_ENGINE(Name, Cls)` 以及旧三参数 `REGISTER_ENGINE_WITH_DEFINITION("...", ...)`；
    3. 虚构节点扫描拦截 `PassthroughNode`、`ComplianceReportPostNode`；
    4. 核心概念完备性扫描覆盖 `doc/architecture.md`、`doc/developer_guide.md` 与 `doc/architecture.puml`；
    5. 校验 `doc/architecture_v2.puml` 具备 `Implemented / Partial / Planned` 状态图例。
  - 新增 `tests/test_architecture_docs_drift_gate.sh` 并在 CTest 注册为 `ArchitectureDocsDriftGateSelfTest`，通过负向测试验证各种漂移均触发失败退出。
  - 修改 `.github/workflows/ci.yml`，彻底移除对 `doc/**` 和 `*.md` 的 `paths-ignore`，确保任何文档或图形资产变更都会触发完整 CI。
- **验证证据**：
  - CTest `ArchitectureDocsDriftTest` 与 `ArchitectureDocsDriftGateSelfTest` 均 100% PASS。

### CR-003 / ACC-R4 (P1): Core / CLI / Web / Runtime 共享非法 Fixture 逐字段一致性矩阵
- **实现方案**：
  - 提取覆盖 9 大典型错误场景的共享 fixture 数据集：
    1. `UNKNOWN_BUSINESS`
    2. `UNKNOWN_NODE_TYPE`
    3. `MISSING_CONFIG_FIELD`
    4. `CONFIG_FIELD_ENUM`
    5. `MODEL_CAPABILITY_MISMATCH`
    6. `DAG_CYCLE`
    7. `MISSING_INPUT_PRODUCER`
    8. `PARALLEL_WRITE_CONFLICT`
    9. `SERIALIZED_ENGINE_CONCURRENCY`（串行 Engine 并发冲突）
  - 在 `tests/test_visualizer_server.py` 中实现 `test_invalid_fixtures_table_driven_parity_matrix`：
    - 真正调用子进程 `alg_pipeline_tool validate --stdin`；
    - 真正发送 HTTP POST 请求至 Visualizer Server `/api/v1/validate`；
    - 逐字段断言 Web API 输出与 CLI 输出 100% 完全相同（`ok`, `diagnostics`, `code`, `path`, `node_id`, `message`, `severity`），证明 Web API 零二次计算、纯透传；
    - 真正调用 `alg_pipeline_tool plan --stdin`，断言非法配置非零退出且 `ok == false`。
  - 在 `tests/test_pipeline_studio.cpp` 中通过 `TableDrivenParityMatrix` 覆盖全部 9 大场景，断言 `PipelineValidator::ValidateAndPlan` 与 `Pipeline::BuildFromJson` 的错误码映射与 Fail-Closed 状态。
- **验证证据**：
  - `python3 tests/test_visualizer_server.py` 6/6 测试（含 9 个子用例）全部 OK。
  - CTest `PipelineStudioTest` 100% PASS。

### CR-005 / ACC-R2 / ACC-R3 (P2): Definition 模式约束校验增强
- **实现方案**：
  - 在 `src/core/pipeline_catalog.cpp` 中增强 `ValidateFieldDefinition`：
    - 仅允许 `kInteger` 与 `kNumber` 带有 `minimum` / `maximum` 范围约束；非数值类型（String, Boolean, Object, Array）若带有 min/max 强制拒绝注册并 Fail-Closed。
  - 在 `PipelineCatalog::RegisterNodeDefinition` 中增加模型绑定 Schema 校验：
    - 若声明了 `model_capability`，则 `model_config_field` 必须非空，且必须在 `config_fields` 中存在并声明为 `kString` 类型。
  - 在 `tests/test_definition_schema_validation.cpp` 中增加对上述非法 Definition 的断言测试。
- **验证证据**：
  - CTest `DefinitionSchemaValidationTest` 100% PASS。

### CR-006 & CR-007 / ACC-R7 (P2): Sanitizer 分级与准确术语规范
- **实现方案**：
  - 增强 `scripts/run_sanitizers.sh`：
    - 支持 `--fast`（默认模式：快速执行核心 CTest 与 Smoke）与 `--full`（全量模式）；
    - 明确日志输出与免责声明：开启 ASan/UBSan，显式标记 `detect_leaks=0`，消除未经 LSan 证明的“零泄漏”断言。
  - 修正 `scripts/run_all_tests.sh` 及所有文档中的描述为“50 轮并发与生命周期稳定 (detect_leaks=0, ASan/UBSan)”。
- **验证证据**：
  - `./scripts/run_sanitizers.sh --fast` 100% PASS。
  - `./scripts/run_all_tests.sh` 6 个阶段全部 100% PASS。

### CR-004 / ACC-R8 (P1): 最终全量门禁与 RFC 交付闭环
- **实现方案**：
  - 在最终候选 SHA 上运行全量 6 阶段自动化回归测试 `./scripts/run_all_tests.sh`，确保 100% PASS。
  - 验证全库 11 个官方 Pipeline JSON 严格通过 `alg_pipeline_tool validate` 与 `plan`。
  - 更新 [RFC-0008](../0008-architecture-contract-consolidation.md) 与 [RFC 索引](../README.md) 状态为 `Completed`。

---

## 3. 全量测试门禁矩阵汇总

| 测试阶段 | 测试内容 | 结果 | 耗时 |
| :--- | :--- | :---: | :--- |
| **Step 1/6** | LayerGuard 架构分层防腐扫描、架构文档防漂移、架构图资产检查、Google C++ 代码规范与 Git Diff 门禁 | **PASS** | ~3.0s |
| **Step 2/6** | CMake 构建与全目标二进制链接 (`libcompany_alg_sdk.so`, 33 个测试程序, CLI) | **PASS** | ~4.5s |
| **Step 3/6** | 核心架构、DAG拓扑排序、引擎容错与全业务细粒度 GTest 单元测试 (Tier 1) | **PASS** | ~8.0s |
| **Step 4/6** | C ABI 安全防御、平台 Operator 接口、8 线程并发、动态热重载与极端边界鲁棒性压测 (Tier 2) | **PASS** | ~2.5s |
| **Step 5/6** | 7 大业务端到端全链路集成测试 (参数化 Profile Demo 套件，Tier 3) | **PASS** | ~1.5s |
| **Step 6/6** | CLI 可视化工具链双模测试 (Python `./show` & 纯 C++ `./build/alg_show`，11 个官方 JSON) | **PASS** | ~0.8s |
| **CTest** | 33/33 CTest 全量套件 (`ctest --output-on-failure`) | **PASS (33/33)** | ~21.3s |
| **Sanitizers** | ASan + UBSan Fast 套件与 Smoke 演示 | **PASS** | ~25s |

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

- **验收结论**：**通过 (Approved - 100% PASS)**
- **建议操作**：更新 RFC-0008 状态为 `Completed`，更新 RFC 索引，执行本地与远端同步。

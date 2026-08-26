# RFC-0008 架构契约收敛交付验收报告

## 1. 结论

**验收通过；RFC-0008 已完成实现与本地门禁，并由标准上传工作流负责远端 CI 和
`main` 合并闭环。**

2026-08-26 本轮修复关闭了再复审报告中的 RCR-001～RCR-006：图形资产现在可从固定
PlantUML 版本、Jar 校验和与源 SHA provenance 生成并 fail-closed 校验；Core、Pipeline、CLI、Web 与 Runtime
共享同一非法 fixture；生产 Definition 与运行时重新一致；负向文档测试不再修改工作
树；sanitizer fast 使用独立构建和 emulator-only 路径；RFC 生命周期恢复为真实状态。

本轮修复起始 HEAD 为 `fb5d4f747a95d44abca0e7b2862610e64349ac40`。最终实现、
测试、本文和 RFC `Completed` 状态位于同一个特性分支交付提交；标准上传脚本只有在
远端 CI 通过并成功合入 `main` 后才返回成功，因此 main 中出现本文即代表交付门禁
已经完成。具体实现提交和 PR/merge SHA 以包含本文的 Git 历史为准，不预填尚未生成的
哈希。

## 2. 验收对象

| 项目 | 值 |
| --- | --- |
| RFC | [RFC-0008](../0008-architecture-contract-consolidation.md) |
| 整改计划 | [剩余整改计划](0008-architecture-contract-consolidation-remediation-plan.md) |
| 修复前再复审 | [2026-08-26 再复审报告](0008-architecture-contract-consolidation-convergence-recheck-20260826.md) |
| 分支 | `feat/architecture-contract-consolidation` |
| 修复起始 HEAD | `fb5d4f747a95d44abca0e7b2862610e64349ac40` |
| 验收对象 | 上述起始 HEAD 加本报告所述收敛修复 |
| RFC 状态 | `Completed` |
| 本地结论 | **PASS** |
| 交付结论 | **APPROVED**（由标准脚本在远端 CI 通过后合入 main） |

## 3. 阻断项关闭证据

### 3.1 RCR-001：图形源与资产一致性

- `scripts/render_architecture_diagrams.sh` 固定 PlantUML `1.2024.7`，并校验 Jar
  SHA-256；工具缺失、下载失败或校验和不匹配均非零退出。
- 权威映射固定为：
  - `doc/architecture.puml` → `doc/assets/architecture_class_diagram.svg`；
  - `doc/architecture_v2.puml` → `doc/assets/architecture_flow.svg`。
- `--generate` 先在临时目录完整渲染和语义检查，写入精确源 SHA 与生成器版本后再安装
  资产；`--check` 重新渲染，并核对已提交资产的 source provenance、SVG 结构和核心
  架构概念，不修改资产，也不依赖跨 CPU/字体栈不稳定的坐标字节。
- `-checkonly` 在隔离的临时源副本执行，避免 PlantUML 在 `doc/` 产生 PNG 副产物。
- `DiagramRenderGateSelfTest` 在临时文档树中验证：源变资产不变会失败、资产损坏会
  失败、`--generate` 可修复且随后 `--check` 通过。

### 3.2 RCR-002：消费者诊断一致性

- 新增单一共享数据集
  `tests/fixtures/pipeline_validation/invalid_pipeline_cases.json`，覆盖 9 类错误：未知
  business、未知 node、未知字段、范围错误、模型能力不匹配、DAG 环、缺失 producer、
  并行写冲突和串行 Engine 并发冲突。
- C++ 矩阵以同一 fixture 驱动 `PipelineValidator::ValidateAndPlan`、
  `Pipeline::BuildFromJson` 和 `SharedAlgorithmRuntime::CreateFromPipelineJson`，断言首个
  `code/path`、必要诊断集合、Pipeline 错误码、失败状态和 C ABI 折叠结果。
- Python 矩阵以同一 fixture 驱动 CLI `validate`、CLI `plan` 与 Studio Web API；非法
  `plan` 保留完整 diagnostics，Web 响应逐字段等于 CLI 响应。

Runtime 对外接口只能暴露折叠后的 C ABI 错误和首诊断文本，因此验收断言的是其公开
可观察映射，不宣称 Runtime 暴露完整 `ValidationReport`。

### 3.3 RCR-003：生产契约与运行时一致

- `VectorSearchNode` 删除未被运行时消费的 `metric=cosine|dot|l2` Definition；当前
  契约只表达实际实现的 cosine 行为。
- `KeywordMatcherNode.default_categories` 恢复 optional，保留“空词表初始化后通过
  Control 注入”的兼容语义。
- 测试错误场景由共享非法 fixture 表达，不再为制造诊断而扭曲生产 Definition。

### 3.4 RCR-004：验收追溯与 RFC 生命周期

- RFC 正文和索引均保持 `In Implementation`。
- 本报告移除了不属于当前历史的旧“最终 SHA”，不将未提交工作区描述成最终交付。
- `Completed`、PR/CI 和 main 同步只在标准分支上传/合并工作流完成后更新。

### 3.5 RCR-005：真正的 sanitizer fast

- `scripts/run_sanitizers.sh --fast` 使用独立 `build-sanitizers-fast` 和独立 ccache；
  禁用 llama.cpp、ONNX Runtime 与真实模型测试，仅构建必要二进制和 12 个核心测试。
- Python/Studio 测试通过环境变量调用 sanitizer 版 `alg_pipeline_tool` 与
  `alg_demo`，不再误用普通 `build/` 产物。
- fast 模式运行 12 项核心 CTest 和 9 个 emulator smoke profile；`--full` 保留完整
  后端与全量 CTest。未知参数返回退出码 2。
- ASan/UBSan 使用 `detect_leaks=0`；本报告不声称 LSan 或“零泄漏”。

### 3.6 RCR-006：负向门禁不污染工作区

- `check_architecture_docs.sh`、`render_architecture_diagrams.sh` 支持受控文档根目录。
- 两个 Shell 自测将 `doc/` 复制到 `mktemp` 目录，只在临时树注入漂移或损坏；
  EXIT/INT/TERM 清理临时目录，不覆盖仓库源文件。

## 4. 实际验证矩阵

| 命令 / 门禁 | 本轮结果 |
| --- | --- |
| `./scripts/format.sh` | PASS |
| `cmake -S . -B build` | PASS |
| `cmake --build build -j4` | PASS |
| `ctest --test-dir build --output-on-failure` | **PASS，33/33，154.23s** |
| 11 个 `configs/pipeline_*.json` 的 `validate` + `plan` | **PASS，11/11** |
| `./scripts/run_all_tests.sh` | **PASS，6/6 阶段** |
| `./scripts/run_sanitizers.sh --fast` | **PASS，12/12 核心 CTest + 9/9 emulator smoke** |
| `python3 tests/test_visualizer_server.py` | **PASS，6/6** |
| `git diff --check` | PASS |

CTest 中 `DiagramRenderGateSelfTest` 真实执行多次 PlantUML 渲染，因此本轮完整 CTest
耗时约 154 秒；不得再使用旧报告中的虚构短耗时。

## 5. 架构不变量核对

1. 六个公开 C ABI 生命周期函数及 C 结构布局未改变，异常防火墙保持。
2. 四层依赖方向保持 `Layer 1 → Layer 2 → Layer 3 → Layer 4`，LayerGuard 通过。
3. `PipelineValidator` 生成唯一 `ValidatedPipelinePlan`；Pipeline 不恢复二次解析或
   DAG 排序。
4. Node/Engine 配置约束共用无副作用验证路径，非法 Definition 在注册写入前拒绝。
5. Node 继续采用浅层 `NodeBase`、`ModelBoundNode`、
   `TraceableUnaryInferenceNode`；请求数据只进入 `AlgContext`。
6. 固定批推理路径保持 `FixedBatchExecutor::Execute` 的 padding、dummy stripping 和
   `(req_id, sub_id)` provenance 契约。
7. 架构图资产由权威 PlantUML 源生成，文档漂移门禁和负向自测均 fail-closed。

## 6. 未验证范围

以下不影响 RFC-0008 的软件交付结论，但不得扩大解释为对应能力已经验证：

- 真实 NPU 硬件：**NOT VERIFIED**；
- TSan、LSan `detect_leaks=1`：**NOT VERIFIED**。

远端提交、PR、CI、合并和 main 同步由
`scripts/git_branch_upload.sh` 在同一命令中执行并在最终状态不一致时返回失败。

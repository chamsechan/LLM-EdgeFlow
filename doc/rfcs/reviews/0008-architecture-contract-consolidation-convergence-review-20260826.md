# RFC-0008 架构契约收敛复审报告（2026-08-26）

## 1. 结论

**当前结论：有显著进展，但尚不能收敛、不能标记 `Completed`，也不建议进入主分支。**

本轮复审确认核心代码方向基本正确，常规构建、31 项 CTest、11 个官方 Pipeline 的
Validate/Plan、六阶段回归和 C ABI 符号检查均通过。不过，当前仍有 4 个 P1 阻断项和
3 个 P2 改进项。最主要的问题不是核心算法失败，而是验收报告把尚未实现的门禁和
尚未覆盖的消费者一致性测试声明为已经完成。

因此，本报告取代
[`0008-architecture-contract-consolidation-acceptance.md`](0008-architecture-contract-consolidation-acceptance.md)
中的 `Approved / 100% PASS` 结论。RFC-0008 应保持 `In Implementation`，直到本文的
P1 全部关闭、最终证据绑定同一候选 Commit，并完成 PR/CI 生命周期。

## 2. 复审对象与环境

| 项目 | 值 |
| --- | --- |
| 分支 | `feat/architecture-contract-consolidation` |
| 复审起始 HEAD | `fd4fcae9373aa4d31c8da83b6f1bafefdc4b7c32` |
| 上游状态 | 相对 `origin/feat/architecture-contract-consolidation` 为 `ahead 6` |
| 起始工作树 | clean；本报告及状态更正是复审产生的未提交文档变更 |
| 系统 | Linux aarch64 / Ubuntu 24.04.1 LTS |
| 编译与格式工具 | GCC 13.3.0、CMake 3.28.3、Clang-Format 18.1.3 |
| 评审依据 | RFC-0008、剩余整改计划、四层架构规则、`llm-edgeflow-developer-guide` |

本轮没有上传、创建 PR 或合并，也没有使用 `github-branch-merge` Skill。

## 3. 已确认的有效进展

以下改造有实现与测试支撑，应继续保留：

1. `ValidationDiagnostic::code` 已使用 `DiagnosticCode` 强类型枚举，Pipeline 的错误
   映射使用 `switch`，对外 JSON 仍输出稳定的大写字符串。
2. Node 与 Engine 的配置值均通过 `ValidateConfigFields` 执行
   required/type/range/enum/unknown 校验，失败发生在模型和节点物化之前。
3. `Pipeline::BuildInternal()` 只消费 `ValidatedPipelinePlan`，没有恢复第二次解析或
   DAG 排序。
4. Node、Engine、Business Definition 的就地注册、业务归属、Engine 线程模型和
   串行模型并发冲突校验方向合理。
5. `INode -> NodeBase -> ModelBoundNode -> TraceableUnaryInferenceNode` 保持浅继承，
   没有按 Pre/In/Post 创建空壳分类基类。
6. common/business 归属已经改善：`LlmGenerateNode` 为 common，DocQA 专用的
   Prompt/Search/Rerank 节点回到业务目录。
7. 固定批 Engine 路径仍通过 `FixedBatchExecutor::Execute`，C ABI 仍只导出六个
   `Alg_*` 生命周期函数。

## 4. 阻断项

### CR-001（P1）：图形资产门禁是空壳，ACC-R6 未关闭

证据：`scripts/render_architecture_diagrams.sh` 的 `--check` 只检查两个 SVG 非空，
`--generate` 只创建目录并输出“up to date”。脚本没有：

- 调用 PlantUML 或 Mermaid；
- 固定生成器版本；
- 从 `doc/architecture.puml` / Mermaid 源生成临时 SVG；
- 比较临时结果与已提交资产；
- 对 `architecture_v2.puml` 做语法检查。

因此 `DiagramAssetsCheckTest` 当前即使 PASS，也无法发现源文件变化后 SVG 未更新。

关闭条件：实现真实、确定性的 `--generate` 和只读 `--check`；将产物渲染到临时目录
后比较；固定工具版本；为“修改源但不更新 SVG”增加必失败的门禁自测。

### CR-002（P1）：文档漂移门禁覆盖不完整，并会被 CI 路径过滤绕过

`scripts/check_architecture_docs.sh` 仍有以下缺口：

- 旧业务名检查没有覆盖 `doc/assets/*.svg`；
- 旧宏检查只处理 `REGISTER_NODE(...)`，没有覆盖旧 `REGISTER_ENGINE(...)` 和旧三参数
  `REGISTER_ENGINE_WITH_DEFINITION("...", ...)`；
- 核心概念只检查 `architecture.md` 与 `developer_guide.md`，没有验证三份架构源文件的
  职责和 As-Is/Target 状态；
- 仅检查 PlantUML/SVG 非空，不检查 PlantUML 语法或源/产物一致性；
- 没有负向自测证明每一类漂移都会让脚本非零退出。

同时 `.github/workflows/ci.yml` 对 `doc/**`、`*.md` 设置了 `paths-ignore`。纯文档、
PlantUML 或 SVG 变更不会触发该工作流，所以即使门禁补全，也可能在最需要它时完全
不运行。

关闭条件：补全扫描范围和负向测试；移除影响架构文档的 `paths-ignore`，或建立独立
的 docs-integrity job，并用一个仅修改架构源文件的 PR 验证工作流会触发。

### CR-003（P1）：CLI/Web/Runtime 一致性矩阵没有按计划落地

`tests/test_pipeline_studio.cpp` 的 `TableDrivenParityMatrix` 当前只执行：

1. `PipelineValidator::ValidateAndPlan()`；
2. 同一报告的 `ToJson()`；
3. `Pipeline::BuildFromJson()` 的失败状态。

它没有调用 `alg_pipeline_tool validate/plan`，也没有访问 Web `/api/v1/validate`。
`tests/test_visualizer_server.py` 只对一个合法配置验证 CLI 和 HTTP API，没有把同一组
非法 fixture 的 `code/path/node_id/port/related_nodes/plan` 与 Core 基准逐字段比较。

关闭条件：把非法 fixture 提取为可复用数据；分别驱动 Core、CLI、Web API 和 Runtime；
比较完整诊断字段和拓扑，而不只是“包含某个 code”或“请求失败”。至少覆盖现有 8 类
错误，并补上整改计划要求的串行 Engine 并发冲突。

### CR-004（P1）：RFC 与验收状态提前完成，证据没有绑定最终交付 SHA

当前分支尚未推送这 6 个提交，也没有 PR/远端 CI/合并证据，但 RFC 和索引已经标为
`Completed`。整改计划的上传/合并步骤仍未勾选；验收报告把最终候选写成
`0240329 (及后续文档闭环提交)`，不是一个可复现的完整 SHA。

关闭条件：先关闭本文其余 P1；在最终候选 SHA 上重跑全部只读门禁；记录精确完整
SHA；用户明确要求交付后按 `github-branch-merge` 工作流提交 PR；CI 通过后才将 RFC
和索引更新为 `Completed`。

## 5. 非阻断但应在本 RFC 内修复的问题

### CR-005（P2）：Definition 自校验仍接受无意义的范围约束

`ValidateFieldDefinition` 会检查 `minimum > maximum`，但没有拒绝 String、Boolean、
Object 或 Array 字段携带 `minimum/maximum`。这种 Definition 可以注册成功，而运行时
又不会执行这些范围约束，形成“声明存在但静默无效”的契约。

建议：只有 Integer/Number 允许 minimum/maximum；为 Node 和 Engine 各增加一个非法
Definition 测试。同时校验声明了 `model_capability` 的 Node 必须引用一个实际存在且为
String 的 `model_config_field`。

### CR-006（P2）：Sanitizer 门禁需要区分快速回归和真实模型长测

本轮在当前候选上复跑 ASan+UBSan：31/31 CTest 通过，但仅 CTest 就耗时
`478.56s`；`QwenEnginesComparisonTest` 为 `201.06s`，`PipelineConfigTest` 为
`261.83s`。存在真实模型时，Smoke 还会继续执行真实 llama.cpp 推理，且相关测试没有
明确超时。原验收报告中的 Sanitizer 总耗时 `~25s` 不能代表当前环境。

建议：

- 快速 Sanitizer gate 默认排除真实模型或显式使用 emulator fixture；
- 真实模型 Sanitizer 作为独立、可选或定时任务，并设置合理超时；
- 报告分别记录构建、CTest、Smoke 用时和是否检测到真实模型；
- 保持 `detect_leaks=0` 时，不得输出“无泄露”或 LSan PASS。

### CR-007（P2）：六阶段脚本仍输出未经门禁证明的“无泄露”结论

`scripts/run_all_tests.sh` 的最终摘要写有“50轮无泄露”，但该脚本没有启用 LSan，
而 Sanitizer 脚本明确使用 `detect_leaks=0`。这与整改计划“不声称零泄漏”的不变量
冲突。

关闭条件：将摘要改成可由测试直接证明的表述，例如“50 轮生命周期测试通过”；只有
独立 `detect_leaks=1` 运行成功时才报告泄漏结论。

## 6. ACC-R1 ~ ACC-R8 复审状态

| 整改项 | 状态 | 复审结论 |
| --- | :---: | --- |
| ACC-R1 诊断枚举 | Closed | 强类型枚举、序列化与 Pipeline 映射已落地 |
| ACC-R2 配置约束 | Closed | Node/Engine 共用配置值校验器 |
| ACC-R3 Definition 自校验 | Conditional | 主体完成，但 CR-005 仍允许静默无效 schema |
| ACC-R4 消费者一致性 | Open | Core/JSON/Runtime 有覆盖，CLI/Web 非法矩阵缺失 |
| ACC-R5 文档漂移门禁 | Open | 扫描不完整且 docs-only 变更被 CI 忽略 |
| ACC-R6 SVG 可重复生成 | Open | 当前渲染脚本没有实际生成或比较 |
| ACC-R7 Sanitizer/ABI | Closed | ABI、ASan+UBSan 31/31 CTest 与 9 个 Smoke Profile 均通过；LSan 未验证 |
| ACC-R8 最终闭环 | Open | RFC 状态、最终 SHA、PR/CI 尚未闭环 |

## 7. 本轮动态验证证据

| 命令/门禁 | 结果 |
| --- | --- |
| `./scripts/format.sh --check` | PASS |
| `./scripts/check_layer_isolation.sh` | PASS |
| `ctest --test-dir build --output-on-failure` | PASS，31/31，7.34s |
| 11 个 `pipeline_*.json` 的 Validate + Plan | PASS，22 次命令均零退出 |
| `./scripts/run_all_tests.sh` | PASS；真实模型使实际耗时显著高于原报告 |
| `nm -D --defined-only build/libcompany_alg_sdk.so` | 仅六个公开 `Alg_*` 生命周期符号 |
| ASan+UBSan CTest | PASS，31/31，478.56s；LSan 未启用 |
| ASan+UBSan Smoke | PASS，9 个 Profile；真实模型 DocQA 单项约 535s，LSan 未启用 |
| 远端 PR/CI | NOT VERIFIED；分支尚未推送本轮 6 个提交 |

注意：`ArchitectureDocsDriftTest` 和 `DiagramAssetsCheckTest` 虽显示 PASS，但因
CR-001/CR-002，其 PASS 只证明当前脚本返回零，不能证明文档与图形无漂移。

## 8. 推荐的最短收敛顺序

1. 修复真实图形生成/比较脚本，并为漂移脚本增加负向自测。
2. 调整 CI 路径过滤，证明 docs-only PR 会运行架构文档门禁。
3. 建立共享非法 fixture，补齐 Core/CLI/Web/Runtime 的逐字段一致性矩阵。
4. 补齐 Definition 非数值范围和 model binding schema 自校验。
5. 分离快速 Sanitizer 与真实模型长测，修正所有“无泄露”和耗时声明。
6. 在同一个最终候选 SHA 上重跑格式、31+ CTest、11×Validate/Plan、六阶段、
   ASan+UBSan、ABI 和文档负向门禁。
7. 更新本报告与验收报告；无 P1 后才进入用户授权的 PR/CI/合并流程。

在 CR-001 ~ CR-004 任一项未关闭前，不得再次将 RFC-0008 标记为 `Completed`。

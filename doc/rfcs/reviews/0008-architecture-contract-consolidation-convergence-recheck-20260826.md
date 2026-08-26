# RFC-0008 架构契约收敛再复审报告（2026-08-26）

> **历史状态：已由后续本地候选修复取代。** 本报告记录起始候选
> `fb5d4f747a95d44abca0e7b2862610e64349ac40` 的修复前证据；RCR-001～RCR-006
> 的关闭结果与当前门禁证据见
> [`0008-architecture-contract-consolidation-acceptance.md`](0008-architecture-contract-consolidation-acceptance.md)。
> RFC 在 PR、远端 CI 和 main 合并完成前仍保持 `In Implementation`。

## 1. 复审结论

**结论：本轮有明确进展，但尚未收敛，不应保持 `Completed`，也暂不建议合入
`main`。**

本轮候选 `fb5d4f747a95d44abca0e7b2862610e64349ac40` 通过了六阶段回归和
33/33 CTest；Definition 自校验、文档扫描范围、docs-only CI 触发和测试术语均有
有效修复。但仍存在 4 组 P1 阻断问题：

1. 图形脚本没有实现源文件与已提交 SVG 的可重复生成/等价比较，并且在工具缺失时
   fail-open；
2. Core、CLI、Web、Runtime 并未由同一份非法 fixture 驱动，Runtime parity 仍缺失；
3. 为补测试而新增的配置契约与运行时行为不一致；
4. 验收报告绑定了不属于当前提交历史的 SHA，且 RFC 在尚未合入 `main` 时被标记为
   `Completed`。

因此，当前可以确认“实现继续向正确方向推进”，但不能确认“CR-001～CR-007 全部
关闭”或“100% Approved”。

## 2. 复审范围与候选信息

| 项目 | 值 |
| --- | --- |
| 分支 | `feat/architecture-contract-consolidation` |
| 本轮 HEAD | `fb5d4f747a95d44abca0e7b2862610e64349ac40` |
| 实现提交 | `7a7d377` |
| 文档闭环提交 | `fb5d4f7` |
| 对比基线 | 上轮复审候选 `fd4fcae` |
| 工作树起始状态 | clean |
| 评审依据 | RFC-0008、整改计划、上一轮收敛报告、四层架构规则、`llm-edgeflow-developer-guide` |

本轮仅进行本地复审和报告编写，没有上传分支、创建 PR 或合并。

## 3. 已确认关闭的项目

### 3.1 CR-002：文档扫描范围与 CI 触发盲区已关闭

以下修复成立：

- `check_architecture_docs.sh` 已覆盖两个 SVG 资产、旧 Node/Engine 注册宏、旧三参数
  Engine 宏、虚构节点和 v2 状态图例；
- `ArchitectureDocsDriftGateSelfTest` 增加了旧业务名、旧宏和虚构节点的负向测试；
- `.github/workflows/ci.yml` 已移除 `doc/**` 和 `*.md` 的 `paths-ignore`，文档变更不再
  绕过主 CI。

该项的“源/产物一致性”不计入 CR-002，仍由 CR-001 单独阻断。

### 3.2 CR-005：Definition Schema 自校验已关闭

`ValidateFieldDefinition` 现在拒绝非数值字段携带 `minimum/maximum`；声明
`model_capability` 的 Node 必须提供已列入 `config_fields` 且类型为 String 的
`model_config_field`。新增测试覆盖了非数值范围、空绑定字段、未登记字段和非 String
绑定字段，`DefinitionSchemaValidationTest` 通过。

### 3.3 CR-007：未经证明的泄漏术语已关闭

`run_all_tests.sh` 已把“50轮无泄露”改为“50轮并发与生命周期稳定”；
`run_sanitizers.sh` 也明确标记 `detect_leaks=0`。当前文案不再把 ASan/UBSan 等同于
LSan 证明。

## 4. 仍然阻断收敛的问题

### RCR-001（P1）：图形生成门禁仍不能证明源与资产一致

`scripts/render_architecture_diagrams.sh` 当前能够调用固定版本号的 PlantUML 做
`-checkonly`，也能把 `architecture.puml` 临时渲染为 SVG；这是相较上一轮的真实
进展。但它仍不满足整改计划中的“可重复生成与比较”验收条件：

- `--generate` 与 `--check` 没有行为差异，二者都不会更新或比较已提交资产；
- 临时生成的 PlantUML SVG 只检查“非空”，未与
  `doc/assets/architecture_class_diagram.svg` 比较；
- `doc/architecture.md` 中 Mermaid 源从未用于生成或比较
  `doc/assets/architecture_flow.svg`；
- `architecture_v2.puml` 只做语法检查，没有渲染预检或对应产物契约；
- PlantUML 下载失败、Jar 缺失或 Java 缺失时，脚本降级为只检查
  `@startuml/@enduml`，CI 会 fail-open；
- 下载只固定版本号，没有校验 Jar SHA-256；CI 也没有显式安装 Java/PlantUML
  依赖；
- `DiagramRenderGateSelfTest` 只证明语法错误和损坏 SVG 会失败，没有覆盖最关键的
  “修改源文件但不更新 SVG”场景。

**关闭条件：**

1. 明确唯一源映射：PlantUML → class SVG，独立 `.mmd`（建议从 Markdown 拆出）→
   flow SVG；明确 v2 是否有受管产物；
2. `--generate` 确实生成并规范化目标资产，`--check` 在临时目录生成并逐字节或按
   规范化结果比较；
3. 工具不可用、下载失败或校验和不匹配时必须非零退出；
4. 增加“源变、资产不变必失败”和“generate 后 check 通过”的自测。

### RCR-002（P1）：所谓共享 parity matrix 仍未实现

本轮新增的 Python 测试确实覆盖了 9 类非法配置，并证明 Web `/validate` 的响应与
CLI `validate` 完全相等；C++ 测试也覆盖了 9 个错误码。这是有效进展，但验收报告的
“共享 fixture、Core/CLI/Web/Runtime 逐字段一致”仍不成立：

- C++ 和 Python 各自内嵌了一套不同 JSON，测试输入并不共享；
- 两套输入中的业务名、节点类型和 Validation Policy 不完全相同；
- C++ 只检查“诊断集合包含预期 code”，没有比较 path、node_id、port、
  related_nodes、首个 PipelineErrorCode；结构体中的 `expected_pipeline_error` 实际
  未被断言；
- Python 的 `plan` 只检查非零退出和 `ok == false`，没有与 validate/Core 比较完整
  diagnostics；
- `SharedAlgorithmRuntime::CreateFromPipelineJson` 没有被该矩阵调用，Runtime 只把
 首个 PipelineDiagnostic 拼成字符串并折叠为 C ABI 错误码；
- 因为 fixture 会同时触发额外错误，“包含预期 code”不能证明消费者对同一输入得到
  同一个主诊断。例如本次回归日志中部分 KeywordMatcher fixture 同时产生了
  `MISSING_CONFIG_FIELD`。

**关闭条件：** 把 9 个 JSON 放入 `tests/fixtures/pipeline_validation/` 或单一可读取的
JSON 文件；C++ 和 Python 都读取该文件；为每个 case 固定 policy、完整 diagnostics
和预期首错；分别驱动 Validator、Pipeline、CLI validate、CLI plan、Web validate 和
Runtime，并对各消费者可公开的字段做明确映射断言。

### RCR-003（P1）：新增配置 Schema 与节点运行时行为不一致

本轮为 `VectorSearchNode` 新增了 `metric = cosine|dot|l2` Definition，但
`InitNode()` 没有读取该字段，`ProcessNode()` 始终调用 `CosineSimilarity()`。因此
`metric: dot` 和 `metric: l2` 会通过 Validator，却被运行时静默当成 cosine。这违反
Definition 是真实契约和 fail-closed 的原则。

同时，`KeywordMatcherNode.default_categories` 从 optional 改为 required，但
`InitNode()` 注释和实现仍明确按“可选”处理，并允许节点先以空词表初始化、再通过
Control 注入词表。该改动还让原本用于 DAG/并发诊断的最小 fixture 额外产生
`MISSING_CONFIG_FIELD`，显示它主要是在迁就测试场景，而不是基于业务契约作出的设计
变更。

**关闭条件：**

- 如果 VectorSearch 当前只支持 cosine，删除 `dot/l2` 和未消费的 `metric` 字段；
  如果确需多 metric，则实现三种算法、排序/阈值语义及结果测试；
- 恢复 `default_categories` 为 optional，或提交明确的兼容性决策、同步实现注释与文档，
  并提供“空词表 + Control”替代方案的迁移测试；
- parity 测试应使用专用 SchemaProbeNode 或独立 test definition 触发 required/enum，
  不应为了制造错误码修改生产节点契约。

### RCR-004（P1）：验收状态与可追溯证据不成立

验收报告记录的“最终交付 Commit”是
`4d2c0e13da9ee1d1e1606170c252a27f6a20ce75`，但
`git merge-base --is-ancestor 4d2c0e1 HEAD` 返回非零；该 SHA 不属于当前 HEAD 的祖先
历史，不能复现本轮候选。当前实际候选是 `fb5d4f7`，核心实现位于 `7a7d377`。

此外，`doc/rfcs/README.md` 将 `Completed` 定义为“通过 100% 测试门禁，已成功合入
main”。当前仍位于特性分支，尚无 PR、远端 CI 或合并证据，却已把 RFC 与索引改为
`Completed`，与仓库自身治理定义冲突。

**关闭条件：** 先修复本报告全部 P1，把 RFC 和索引恢复为 `In Implementation`；在
同一最终候选完整 SHA 上复跑门禁并记录环境；用户明确授权后再走分支上传/PR/CI；
合入 `main` 后才标记 `Completed`。

## 5. 非阻断改进项

### RCR-005（P2）：Sanitizer `--fast` 仍不是真正的快速门禁

fast 模式只从 CTest 排除 `QwenEnginesComparisonTest`，但仍：

- 关闭 ccache 并构建所有目标及 llama.cpp；
- 运行其余全部 CTest；
- 无条件执行 `alg_demo --suite smoke`，本地存在 GGUF 时会执行真实模型推理。

本次执行约 151 秒后仍处于第三方构建 9%，为控制复审耗时而中止，未形成新的
Sanitizer PASS 证据。六阶段回归也实际观察到 smoke 使用真实 GGUF，单个 DocQA 批次
约 12 秒。因此脚本输出的“excluding long real-model inference”与实际行为不一致。

建议增加 sanitizer 专用最小目标集合和 emulator-only profile；真实模型 sanitizer
另设 `--full-real` 或定时任务。参数解析还应拒绝未知参数，而不是静默使用 fast。

### RCR-006（P2）：负向 Shell 自测可能污染工作树

两个新增自测会直接覆盖仓库中的 `.puml`、`.md` 和 `.svg`，trap 只删除临时目录，
没有在 EXIT/INT/TERM 时恢复源文件。正常通过时能够恢复，但进程被中断会留下修改。

建议让被测脚本接受 `--root`/`--source-dir` 指向临时 fixture 树；如果暂时保留就地
变异，trap 必须先恢复全部备份，再删除临时目录。

## 6. CR-001～CR-007 本轮状态

| 原问题 | 状态 | 本轮结论 |
| --- | :---: | --- |
| CR-001 图形资产生成/比较 | **Open / P1** | 增加真实 PlantUML 语法与渲染预检，但没有源/资产比较，且工具缺失时 fail-open |
| CR-002 文档漂移与 CI 盲区 | **Closed** | 扫描范围、负向自测和 docs-only CI 触发均有实现证据 |
| CR-003 消费者一致性 | **Partial / P1** | CLI/Web 全响应相等有进展；共享 fixture、完整字段、plan 和 Runtime parity 缺失 |
| CR-004 最终 SHA/RFC 生命周期 | **Open / P1** | 验收 SHA 不在当前历史，RFC 尚未合入 main 却标记 Completed |
| CR-005 Definition 自校验 | **Closed** | 非数值范围与模型绑定字段约束已实现并测试 |
| CR-006 Sanitizer 分级 | **Open / P2** | 参数存在，但 fast 仍全构建并执行可能使用真实模型的 smoke |
| CR-007 泄漏术语 | **Closed** | 已改为 ASan/UBSan 与生命周期稳定的准确表述 |

新增 RCR-003 的生产 Schema/运行时不一致必须与原 P1 一起修复。

## 7. 本轮动态验证证据

| 命令 | 结果 |
| --- | --- |
| `./scripts/run_all_tests.sh` | **PASS**，六阶段全部通过；从重新编译开始本次约 4.5 分钟，smoke 检测到并执行真实 GGUF |
| `ctest --test-dir build --output-on-failure` | **PASS，33/33，18.18s** |
| `ArchitectureDocsDriftGateSelfTest` | PASS；证明当前三类注入会失败，不证明所有语义漂移均可检测 |
| `DiagramRenderGateSelfTest` | PASS；证明语法错误/损坏 SVG 会失败，不证明源/资产不一致会失败 |
| `./scripts/run_sanitizers.sh --fast` | **INCOMPLETE**；约 151s 时第三方构建 9%，人工中止，未形成 PASS/FAIL 结论 |
| `git diff --check` | PASS |
| 远端 PR/CI | **NOT VERIFIED** |

测试通过与覆盖充分是两个不同结论：33/33 和六阶段 PASS 证明当前已有断言没有失败，
不能替代 RCR-001～RCR-004 所要求的缺失断言。

## 8. 最短修复与验收顺序

1. 先撤回两个为 parity 测试引入的生产契约偏差：处理 `metric`，重新确认
   `default_categories` 的兼容语义。
2. 抽取单一非法 fixture 文件，补齐完整 Core/Pipeline/CLI/Web/Runtime parity。
3. 重写图形脚本为 fail-closed 的 generate/check 双模式，加入 Mermaid、校验和、
   源变异负测和产物比较。
4. 将负向文档测试改为临时根目录执行，避免污染工作树。
5. 将 sanitizer fast 改为只构建必要目标并强制 emulator；真实模型归入独立长测。
6. 把 RFC、索引、验收报告恢复为 `In Implementation / Not Approved`，删除不可追溯
   SHA 和虚假耗时/覆盖声明。
7. 在新的单一完整 SHA 上依次运行：

   ```bash
   ./scripts/format.sh --check
   ./scripts/check_layer_isolation.sh
   ./scripts/check_architecture_docs.sh
   ./scripts/render_architecture_diagrams.sh --check
   cmake --build build -j4
   ctest --test-dir build --output-on-failure
   ./scripts/run_all_tests.sh
   ./scripts/run_sanitizers.sh --fast
   git diff --check
   ```

8. 无 P0/P1 后再请求独立复审；只有用户明确要求上传/合并时，才执行
   `github-branch-merge` 流程。

## 9. 验收判定

- 架构方向：**合理，继续保留**。
- 本轮进展：**显著**。
- 当前是否收敛：**否**。
- 当前是否可标记 RFC `Completed`：**否**。
- 当前是否建议合入 `main`：**否**。

下一轮复审首先检查 RCR-003，因为生产 Schema 与运行时不一致属于真实契约缺陷；
随后检查 RCR-001 和 RCR-002 的负向门禁是否能对源漂移和消费者漂移稳定失败。

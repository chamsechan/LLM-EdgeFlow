# RFC-0008 架构契约收敛剩余整改计划

## 1. 文档定位

本文是
[`RFC-0008`](../0008-architecture-contract-consolidation.md)
的配套实施计划，只覆盖当前审查后仍未关闭的工作，不创建第二套架构设计，也不替代
RFC 本身。

2026-08-26 的完成状态复核、阻断项证据与最短收敛顺序见
[`0008-architecture-contract-consolidation-convergence-review-20260826.md`](0008-architecture-contract-consolidation-convergence-review-20260826.md)。

执行目标是让 RFC-0008 从当前的 `In Implementation` 达到可以独立验收、提交 PR
并合并的状态。本文中的勾选项必须以代码、测试或命令输出为证据，不得仅根据“已有
Smoke 通过”推断完成。

本文不授权推送、创建 PR 或合并。只有用户明确要求交付时，才执行仓库规定的
`github-branch-merge` 工作流。

## 2. 当前基线与已验证进展

| 项目 | 当前值 |
| --- | --- |
| 工作分支 | `feat/architecture-contract-consolidation` |
| 已提交候选 | `fd4fcae9373aa4d31c8da83b6f1bafefdc4b7c32`（2026-08-26 复审起始 HEAD，仍不是最终交付 SHA） |
| 工作区状态 | 已产生收敛复审文档与状态更正；不得把上述 Commit 当成最终实现 SHA |
| 生产节点 | 27 个（1 个 common，26 个 business） |
| 官方 Pipeline JSON | 11 个 |
| CTest | 31 项 |
| 最近常规验证 | 格式化、默认构建、31/31 CTest、11 个严格 Validate/Plan、六阶段回归均通过；文档/SVG 门禁仍有复审阻断项 |
| RFC 状态 | `In Implementation` |

已经确认合理并应保留的设计包括：

- 四层依赖方向 `Layer 1 → Layer 2 → Layer 3 → Layer 4`；
- Adapter 发布 Business Definition，Node/Engine 与 Definition 就地注册；
- Typed `BlackboardKey<T>`、`ValidatedPipelinePlan` 和严格生产校验策略；
- `INode → NodeBase → ModelBoundNode → TraceableUnaryInferenceNode` 的浅继承；
- 业务节点显式 `business_names`、Engine `thread_model` 和串行实例并发冲突校验；
- `LlmGenerateNode` 为 common，DocQA 的 Prompt/Search/Rerank 节点为 business；
- `DenseRetrievalNode` 使用 Embedding 参与实际召回。

历史“改造前基线”没有在变更前完整冻结，因此不得补造数据。阶段 0 只冻结当前候选
工作区，并在最终验收报告中明确这一限制。

## 3. 剩余问题与优先级

| 编号 | 等级 | 问题 | 关闭条件 |
| --- | :---: | --- | --- |
| ACC-R1 | P1 | Validator 内部仍以字符串保存和比较诊断码，Pipeline 通过字符串分支映射错误码 | 引入 `DiagnosticCode` 枚举；内部零字符串判断；外部 JSON 字符串保持兼容 |
| ACC-R2 | P1 | `ConfigFieldDefinition::required`、`enum_values` 没有完整执行；Engine 数值范围也未统一校验 | Node/Engine 共用一个无副作用配置校验器，并覆盖 required/type/range/enum/unknown |
| ACC-R3 | P1 | Definition 自身可能包含重复字段、非法默认值或无效范围，注册期没有 fail-closed | Catalog/Registry 在构造实例前拒绝无效 Definition，并有零副作用测试 |
| ACC-R4 | P1 | CLI、Web、Pipeline 和 Runtime 尚缺同一错误的代码/路径/拓扑一致性证据 | 表驱动测试验证各消费者共享 Validator 输出，不维护 UI 侧校验副本 |
| ACC-R5 | P1 | 架构文档漂移仍依赖人工搜索，旧业务名、旧宏或虚构节点可能再次进入文档 | 新增只读漂移脚本并注册 CTest、六阶段回归和 CI |
| ACC-R6 | P2 | 两个 README SVG 缺少固定工具版本和可重复生成流程 | 明确源文件到资产映射，提供生成与 `--check` 模式，禁止继续手工改 SVG |
| ACC-R7 | P2 | 本轮尚未取得当前候选 SHA 的 Sanitizer 证据 | ASan/UBSan 通过或记录可复现的环境级 `NOT VERIFIED`；不得声称零泄漏 |
| ACC-R8 | P1 | 缺少绑定最终提交 SHA 的独立 RFC-0008 验收报告 | 完成静态审查、完整门禁、验收报告、RFC 状态及索引闭环 |

## 4. 不得破坏的设计不变量

1. 不修改六个公开 C ABI 函数签名、C 结构布局、枚举数值和 `noexcept` 异常边界。
2. 不改变现有 `PipelineErrorCode` 的枚举顺序或数值；新的 Validator 诊断枚举必须独立。
3. `ValidationReport::ToJson()` 的 `diagnostics[].code` 继续输出现有大写字符串，避免破坏
   CLI、Web 和外部工具。
4. `PipelineValidator` 继续是唯一无副作用预检入口。Web、CLI、Runtime 不得各自复制
   required/enum/range 或并发规则。
5. `Pipeline::BuildInternal()` 只消费一次 `ValidatedPipelinePlan`，不得恢复第二次解析、
   Registry 校验或 DAG 排序。
6. 校验失败发生在 Engine 创建、模型加载、Node 创建和 Node Init 之前。
7. common 节点不得包含 `business/` 头文件；Core 不得依赖业务 DTO、Adapter 或具体 Engine。
8. Definition 默认值本阶段不自动写回 Pipeline JSON，也不改变 Node/Engine 现有运行时
   默认值语义。若要统一默认值物化，另立 RFC，避免本次范围膨胀。
9. 不提交第三方 Jar、npm 包、预编译二进制或生成工具源码。
10. RFC 在实现、Sanitizer/验收证据和最终交付闭环前保持 `In Implementation`。

## 5. 分阶段实施计划

### 阶段 0：冻结候选工作区并建立可回滚检查点

目标：避免在大规模未提交修改上继续叠加而失去问题归因能力。

- [ ] 记录 `git branch --show-current`、`git rev-parse HEAD`、`git status --short` 和
      `git diff --stat`。
- [x] 人工确认工作树中的所有修改均属于 RFC-0008；用户文件不得因格式化或重命名被
      意外覆盖。
- [x] 再次执行 `git diff --check`，确认无冲突标记和空白错误。
- [x] 形成当前候选实现的检查点提交；建议提交：
      `refactor(architecture): consolidate runtime contracts and node support`。
- [x] 检查点提交后重新记录完整 SHA，后续测试证据均引用该 SHA 或其后续提交。

完成标准：工作树变化有明确归属，能够按提交回退；不得使用 `git reset --hard` 或
`git checkout --` 丢弃用户修改。

### 阶段 1：将结构化诊断收敛为稳定枚举（ACC-R1）

#### 1.1 目标接口

建议在 `include/core/pipeline_validator.h` 中定义：

```cpp
enum class DiagnosticCode {
  kOk,
  kJsonParse,
  kConfigFileOpen,
  kRootType,
  kUnknownField,
  kMissingField,
  kFieldType,
  kFieldRange,
  kInvalidCombination,
  kDuplicateModelId,
  kDuplicateNodeId,
  kUnknownBusiness,
  kUnknownNodeType,
  kUnknownEngineType,
  kInvalidDependency,
  kDuplicateDependency,
  kDagCycle,
  kRegistryConflict,
  kUnknownConfigField,
  kMissingConfigField,
  kConfigFieldType,
  kConfigFieldRange,
  kConfigFieldEnum,
  kUnknownModelReference,
  kModelCapabilityMismatch,
  kNodeBusinessMismatch,
  kMissingInputProducer,
  kDuplicatePortProducer,
  kMissingBusinessOutput,
  kNodeNotParallelSafe,
  kParallelWriteConflict,
  kSerializedEngineConcurrency,
  kInternalException,
};

const char* DiagnosticCodeName(DiagnosticCode code) noexcept;
```

具体枚举可以拆到 `include/core/validation_diagnostic.h`，但只能有一个定义和一个字符串
序列化函数。

#### 1.2 修改步骤

- [x] 将 `ValidationDiagnostic::code` 从 `std::string` 改为 `DiagnosticCode`。
- [x] 将 Validator 内部 `Add()` 改为接收 `DiagnosticCode`。
- [x] 把 `PipelineErrorCodeName()` 替换为无字符串中转的
      `PipelineErrorCode → DiagnosticCode` 映射。
- [x] 将 `src/core/pipeline.cpp` 的 `ValidationCodeToPipelineCode(string)` 改为
      `switch (DiagnosticCode)`；不修改既有 `PipelineErrorCode` 数值。
- [x] `ValidationReport::ToJson()`、CLI 错误包装和日志统一调用
      `DiagnosticCodeName()`，外部仍看到例如 `DAG_CYCLE`。
- [x] 更新 `alg_pipeline_tool normalize`，禁止把枚举隐式当字符串传递。
- [x] 更新测试中的字符串比较：内部断言比较枚举，JSON/API 断言比较序列化字符串。
- [x] 全仓搜索并清除生产代码中的 `diagnostic.code == "..."`、
      `item.code + ...` 和基于错误消息文本的兼容判断。

#### 1.3 测试

- [x] 在 `tests/test_validated_pipeline_plan.cpp` 增加枚举到字符串的表驱动测试，确保
      名称唯一且稳定。
- [x] 在 `tests/test_pipeline_config.cpp` 验证代表性 Validator 诊断到
      `PipelineErrorCode` 的映射。
- [x] 在 `tests/test_pipeline_studio.cpp` 验证 JSON 中的 code/path/message 与当前
      对外格式完全一致。
- [x] 覆盖未知兜底；生产路径不得输出空 code。

阶段完成标准：内部无诊断码字符串分支，CLI/Web JSON 无兼容性变化，受影响测试和
全部 11 个 Pipeline Validate/Plan 通过。

建议提交：`refactor(validation): introduce stable diagnostic codes`。

### 阶段 2：完成 Definition 配置约束（ACC-R2、ACC-R3）

#### 2.1 单一配置校验器

在 `src/core/pipeline_validator.cpp` 内先实现一个无副作用 helper；只有两个以上编译单元
需要时才提升为 Core 公共接口。建议输入为 Definition 字段数组、实际 config、JSON
Pointer 前缀和可选 node id，统一处理：

1. 未知字段：`kUnknownConfigField`；
2. 缺失 required 字段：`kMissingConfigField`，路径指向
   `<base>/config/<field-name>`；
3. 类型错误：`kConfigFieldType`；
4. 数值越界：`kConfigFieldRange`；
5. 字符串不在 enum 中：`kConfigFieldEnum`。

Node 和 Engine 必须调用同一个 helper。Engine 当前缺失的 minimum/maximum 校验要在
此阶段补齐。

#### 2.2 Definition 自校验

- [x] 在 `PipelineCatalog::RegisterNodeDefinition` 和
      `RegisterEngineDefinition` 写入 Catalog 前校验 Definition。
- [x] 拒绝空字段名和同一 Definition 内重复字段名。
- [x] 拒绝 `minimum > maximum`。
- [x] 拒绝 default 的 JSON 类型与 `kind` 不一致。
- [x] 拒绝 default 超出范围或不在 enum 中。
- [x] `enum_values` 仅允许用于 `kString`；枚举值自身不得重复。
- [x] required 表示必须由 Pipeline 显式提供；default 不替代 required。
- [x] Registry 收到无效 Definition 时保持 fail-closed，Creator 不得写入 Factory。
- [x] Business 批量注册继续保持原子性。

不要在注册锁内调用外部 Creator，也不要在 Validator 中创建 Engine/Node 验证配置。

#### 2.3 测试

- [x] 新增 `tests/test_definition_schema_validation.cpp` 并在 CMake 注册独立 CTest，避免
      Registry 冲突状态污染其他套件。
- [x] 表驱动覆盖 Node/Engine 的 required、type、min、max、enum、unknown。
- [x] 覆盖非法 Definition：重复字段、错误 default、倒置范围、重复 enum。
- [x] 使用 create/init 计数探针证明任何预检失败均为零实例化、零模型加载、零 Node Init。
- [x] 验证多个错误的顺序稳定：models 按配置顺序、nodes 按 Pipeline 顺序、字段按
      Definition 声明顺序；不要依赖错误消息自然语言排序。
- [x] 对全部生产 Definition 做 Catalog 自检，确保没有无效元数据。

阶段完成标准：Definition 声明的全部约束都有执行语义；Node/Engine 规则无重复实现；
官方 11 个配置仍全部严格通过。

建议提交：`fix(validation): enforce definition config constraints`。

### 阶段 3：统一 CLI、Web、Runtime 的验证结果（ACC-R4）

- [x] 为以下错误各准备一份最小 JSON fixture：未知业务、未知节点、缺失 required、
      enum 错误、能力不匹配、DAG 环、缺失 producer、并行写冲突、串行 Engine 并发冲突。
- [x] 直接调用 `PipelineValidator::ValidateAndPlan()` 取得基准报告。
- [x] 调用 `alg_pipeline_tool validate`，断言 `code/path/related_nodes/port` 与基准一致。
- [x] 调用 Studio Validator API，断言它只转发 Validator JSON，没有 JavaScript 规则副本。
- [x] 调用 `Pipeline::BuildFromJson`，断言首个 Validator 诊断映射为预期
      `PipelineErrorCode`，且 Pipeline 状态为 `kFailed`。
- [x] 对带 side-effect probe 的配置确认 Runtime 失败前没有创建模型或初始化节点。
- [x] `plan` 成功输出必须与 `ValidatedPipelinePlan` 的 order/layers 一致。

优先扩展 `tests/test_pipeline_studio.cpp`、`tests/test_pipeline_config.cpp` 和
`tests/test_validated_pipeline_plan.cpp`；若 fixture 被三处复用，再提取到
`tests/fixtures/pipeline_validation/`，不要先创建无用途的测试框架。

阶段完成标准：同一配置在 Core、CLI、Studio 和 Build 路径上只有表现层差异，没有
规则、代码、路径或拓扑差异。

建议提交：`test(validation): lock consumer diagnostic parity`。

### 阶段 4：建立文档漂移与图形资产门禁（ACC-R5、ACC-R6）

#### 4.1 文档职责固定

- [x] `doc/architecture.md`：当前运行架构概念、数据流和开发入口。
- [x] `doc/architecture.puml`：当前稳定结构关系，不维护完整 Node/Engine 手工清单。
- [x] `doc/architecture_v2.puml`：目标蓝图，所有非当前能力标记 `Planned`，部分能力标记
      `Partial`。
- [x] `PipelineCatalog` JSON：Node、Engine、Business 精确清单的唯一事实源。
- [x] 修正 `doc/README.md` 中“精确镜像所有成员”的表述，使其与简化 As-Is 图一致。

#### 4.2 只读漂移检查

新增 `scripts/check_architecture_docs.sh`，默认只读并在失败时返回非零。至少检查：

- [x] 权威文档和 SVG 中不存在旧业务名
      `doc_qa_embedding_v1`、`doc_qa_rerank_v1`；
- [x] 不存在旧注册形式 `REGISTER_NODE(`、`REGISTER_ENGINE(` 或三参数
      `REGISTER_ENGINE_WITH_DEFINITION("...", ...)`；
- [x] 不存在虚构生产节点 `PassthroughNode`、`ComplianceReportPostNode`；测试探针
      `CountingNode` 只能出现在 tests/review 历史证据中；
- [x] 三份架构文档包含 `ValidatedPipelinePlan`、`BlackboardKey`、`NodeBase` 和
      `FixedBatchExecutor` 的正确职责；
- [x] `architecture_v2.puml` 同时包含 `Implemented`、`Partial`、`Planned` 状态图例；
- [x] 根 README 中的测试数、Pipeline 数或节点数若保留，必须由检查脚本与当前资产
      动态比对；更推荐删除易漂移的硬编码数量。

将脚本注册为 `ArchitectureDocsDriftTest`，接入：

- `CMakeLists.txt`；
- `scripts/run_all_tests.sh`；
- `.github/workflows/ci.yml`。

#### 4.3 SVG 可重复生成

明确以下映射：

| 资产 | 权威源 | 生成器 |
| --- | --- | --- |
| `doc/assets/architecture_flow.svg` | `doc/architecture.md` 中指定 Mermaid block，或拆出的独立 `.mmd` | 固定版本 Mermaid CLI |
| `doc/assets/architecture_class_diagram.svg` | `doc/architecture.puml` | 固定版本 PlantUML |

- [x] 新增 `scripts/render_architecture_diagrams.sh`，支持真实生成和 `--check` 两种模式。
- [x] 工具版本必须固定；依赖下载到构建缓存或临时目录，不提交 npm 包、Jar 或二进制。
- [x] `--check` 渲染到临时目录后比较，不直接修改工作区。
- [x] 消除时间戳、随机 ID、绝对路径等非确定性输出后再接入 CI。
- [x] `architecture_v2.puml` 暂无 README SVG 消费者时只做 PlantUML 语法检查；若新增
      SVG，则必须加入同一映射和漂移检查。
- [x] 一旦生成链稳定，禁止继续手工编辑两个 SVG。

阶段完成标准：修改权威源但未更新资产、重新引入旧标识或写错状态时，本地 CTest 和
CI 都会失败。

建议提交：`test(docs): add architecture drift and render gates`。

### 阶段 5：Sanitizer、ABI 与独立静态复验（ACC-R7）

按由窄到宽的顺序执行，失败时保存首个失败命令、退出码和日志摘要，不要直接把环境
问题解释为代码问题，也不要把工具初始化失败写成 PASS。

```bash
LLM_EDGEFLOW_SANITIZERS=undefined ./scripts/run_sanitizers.sh
./scripts/run_sanitizers.sh
```

- [x] UBSan：全部已注册 CTest 和 Smoke 通过。
- [x] ASan+UBSan：全部已注册 CTest 和 Smoke 通过。
- [x] 当前脚本设置 `detect_leaks=0`，因此通过也不能声称 LSan 或“零泄漏”。
- [x] 若需要泄漏结论，在支持 LSan 的 Linux 环境单独以 `detect_leaks=1` 复验并记录。
- [x] 若 Sanitizer runtime 在项目代码前失败，用最小空程序复现后标记
      `NOT VERIFIED (environment)`，不得标记成功。
- [x] 运行 `./scripts/check_layer_isolation.sh`，并人工复查 Core、common nodes、Adapter
      和具体 Engine 的 include 方向。
- [x] 运行 C11 ABI 测试并检查最终动态库仅保留预期六个公开 `Alg_*` 生命周期符号。
- [x] 检查所有固定批实现继续通过 `FixedBatchExecutor::Execute`。
- [x] 检查 Engine/Node 注册宏、Definition 和类常量名称一致。

将证据写入新的
`doc/rfcs/reviews/0008-architecture-contract-consolidation-acceptance.md`，并绑定最终候选
Commit SHA、工具版本、系统、CPU、构建类型和可用模型。无法验证的真实硬件、TSan、
LSan 或远端 CI 必须单列 `NOT VERIFIED`。

建议提交：`docs(review): record RFC 0008 acceptance evidence`。

### 阶段 6：最终全量门禁与交付（ACC-R8）

最终代码变化完成后，从干净或明确记录的工作树执行：

```bash
./scripts/format.sh --check
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure

for config in configs/pipeline_*.json; do
  ./build/alg_pipeline_tool validate "$config"
  ./build/alg_pipeline_tool plan "$config"
done

./scripts/run_all_tests.sh
./scripts/check_layer_isolation.sh
git diff --check
```

- [x] 所有命令零退出；CTest 必须动态报告全部注册测试通过，不能硬编码旧数量。
- [x] 7 个业务 Smoke 全部产生结构化结果并通过断言。
- [x] Catalog 中 Node/Engine/Business Definition 与 Factory/Adapter 注册集合一致。
- [ ] 审查报告无 P0/P1；P2 有责任、复验命令和明确状态。
- [ ] README、开发指南、Skill、三份架构文档、SVG 和 RFC 索引与代码一致。
- [x] 提交后复跑只读格式与快速 CTest，保证验证对象与最终 SHA 相同。
- [x] 保持 RFC 为 `In Implementation` 直至全部阶段完成，并在验收后更新为 `Completed`。
- [ ] 用户明确要求上传/合并后，读取 `github-branch-merge` Skill，并且只使用
      `./scripts/git_branch_upload.sh "<commit message>" "<type>"` 执行标准工作流。
- [ ] PR/CI 通过后更新 RFC 和索引为 `Completed`，合并后验证 `main` 与远端同步且
      工作区干净。

## 6. 推荐提交序列

```text
refactor(architecture): consolidate runtime contracts and node support
refactor(validation): introduce stable diagnostic codes
fix(validation): enforce definition config constraints
test(validation): lock consumer diagnostic parity
test(docs): add architecture drift and render gates
docs(review): record RFC 0008 acceptance evidence
docs(rfc): complete architecture contract consolidation
```

每个提交必须能够单独构建。不要把诊断枚举迁移、配置约束和文档生成工具压成一个
超大提交，否则失败时无法区分 API 迁移、行为变化和工具链问题。

## 7. 每阶段最小验证矩阵

| 阶段 | 必跑测试 | 进入下一阶段的条件 |
| --- | --- | --- |
| 0 | `git diff --check`、当前 28 项 CTest | 检查点可回滚，基线 SHA 已记录 |
| 1 | Validated Plan、Pipeline Config、Studio、CLI Validate/Plan | 内部零字符串诊断分支，外部 JSON 不变 |
| 2 | Definition Schema、Catalog SSOT、Pipeline Config | 全部字段约束生效且预检零副作用 |
| 3 | Studio、CLI、Runtime parity fixtures | code/path/plan 一致 |
| 4 | ArchitectureDocsDriftTest、SVG `--check`、LayerGuard | 修改权威源会触发漂移门禁 |
| 5 | UBSan、ASan+UBSan、C11 ABI、LayerGuard | PASS 或有可复现的环境级 NOT VERIFIED |
| 6 | 格式、构建、全部 CTest、11×Validate/Plan、六阶段回归 | 无 P0/P1，证据绑定最终 SHA |

## 8. 验收时必须回答的问题

1. Validator 是否仍有任何基于字符串或错误消息的控制流？
2. Node 与 Engine 的 required/type/range/enum 是否走同一实现？
3. 无效 Definition 是否在 Factory/Catalog 写入前被拒绝？
4. 同一非法配置在 Core、CLI、Web 和 Runtime 是否产生相同 code/path？
5. 失败预检是否严格零模型加载、零节点构造和零节点初始化？
6. 文档能否通过自动门禁阻止旧宏、旧业务名和虚构节点回归？
7. SVG 是否可从固定版本工具和权威源重新生成？
8. Sanitizer 结论是否准确区分 PASS、FAIL 和 NOT VERIFIED？
9. C ABI、四层依赖、Fixed Batch 和请求无状态不变量是否保持？
10. 最终报告、RFC、README、PR 与测试证据是否绑定同一个 Commit SHA？

上述任一 P1 问题未关闭时，不得把 RFC-0008 标记为 `Completed`，也不得合并主分支。

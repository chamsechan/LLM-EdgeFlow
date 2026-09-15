# 上线前代码与测试精简实施计划

- 审查日期：2026-09-15。
- 源码基线：`6a105fee97a83859df97c1ca8e1cdaf344d9eff8`。
- 前提：框架尚未上线，可一次迁移仓内消费者，不必为旧接口长期保留双路径。
- 交付范围：审查结论与实施计划；本文建议尚未实施，不代表相关 RFC 已通过或完成。
- 文中行号对应上述基线；实施时用文件链接及符号定位。当前架构约束以 [AGENTS](../../AGENTS.md) 为准，实施流程以 [CONTRIBUTING](../../CONTRIBUTING.md) 为准。

## 1. 结论与建议顺序

**仍有值得精简的代码和测试，但上一轮主要兼容入口已经清理。下一轮应先修测试漏选，再删除确认无用的状态和包装、合并相同职责，最后集中调整仍在使用的历史契约。**

主要收益是减少读代码时需要理解的分支、接口和重复声明。尚无删除前后的性能或体积测量，不能承诺 SDK 明显变小或推理明显变快。测试精简也不能以“用例数下降”作为目标。

建议按下表分批实施。优先级表示顺序，不是安全问题等级。预估为熟悉项目的一名开发者的工作量，含聚焦验证、不含真实模型资产或硬件准备；仅供排期。

| 批次 | 内容 | 建议工时 | 完成标志 | 实施状态 |
| --- | --- | --- | --- | --- |
| 0 | V1：修复默认 runner 漏选，建立实际用例清单核对 | 0.5–1 天 | 所有应运行的编译用例都能映射到 CTest | 已完成 |
| 1 | A1–A4、T1–T3、T6：局部精简与明确重复测试清理 | 1–2 天 | 无用状态/包装消失，行为和独特断言保留 | 已完成 |
| 2 | B1–B3、T4–T5：共享实现和测试归属整理 | 2–3 天 | 同一规则只维护一次，边界语义不变 | 已完成 |
| 3 | C1–C4：Adapter、计划结构、验证策略、测试布局的契约收口 | 各 1–3 天，分别设计 | RFC 明确迁移范围；旧入口和消费者同批迁移 | 待后续按 RFC 实施 |
| 暂缓 | D1–D3：Model 查询、BGE 复制、其他开发入口 | 不预排 | 先证明收益，必要时才实施 | 暂缓 |

批次 3 不是批次 1/2 的交付前置条件。批次 0、1、2 已按计划完成实施与验证；批次 3 待按实际收益选择契约调整，不把全部建议塞进一个大重构。

## 2. 审查范围与证据边界

覆盖接入适配层 / Integration、流程编排层 / Orchestration、能力节点层 / Capability Nodes、模型执行层 / Model Execution，以及 Demo、Studio、脚本、CMake 和测试。采用分层只读审查、符号引用检索、控制流核对、测试刺激与断言对照，以及已有构建的 Catalog/测试清单查询。

本基线 `src/` 有 110 个受版本管理文件，`include/` 有 79 个，`tests/` 有 96 个。按常见代码后缀统计，测试代码约 41,015 行，包含注释、空行和 shell/Python 测试；这些数字不是可删除行数。对审查目录内超过 100 字节的常见代码文件做内容哈希，未发现整文件完全相同的重复组，主要问题是函数和职责级重复。

实际查询已有 `./build/alg_pipeline_tool catalog`，返回 `ok=true`、`schema_version=3`，含 12 个 Node、6 个 Model、8 个 biz、14 个 Profile；此构建有 `llama_cpp`、`onnxruntime` 两个 Backend。注册模型存在不代表该构建拥有其所需 Backend 或模型资产。首次查询使用已有二进制，不能单凭它证明源码已经重新构建。

“无调用”仅指本仓库基线，包含生成器、测试、开发支持代码的检索，不涵盖仓外扩展。接口已被现行文档、脚手架或测试当作扩展契约使用时，本文不将其当作可以直接删除的死代码。未接触公司内部 SDK，也未进行真实模型效果、目标硬件或非默认 Backend 验收。

## 3. 已完成的清理，不再安排实施

核对 [RFC-0056](../rfcs/0056-batch-1-framework-slimming.md)、[RFC-0058](../rfcs/0058-diagnostic-and-node-registry-convergence.md) 及当前源码：

| 旧审查事项 | 当前结果 |
| --- | --- |
| 七个旧公共头、`NodeFactory` 别名 | 已删除 |
| PromptGuided 私有模板解析器、`template_syntax`、单括号替换兼容 | 已清理；当前双括号替换、单括号为字面文本 |
| Operator `mem_que` 与 `data.outputs` 双配置路径 | 已统一为 `data.outputs` |
| Bridge 顶层输出回调、Adapter 标识回退、隐式输出 key | 已收口，并完成 typed helper 的自定义槽回归修复 |
| `--no-default-control`、Studio 旧 Catalog 字段消费 | 已删除 |
| Core 两套诊断枚举与往返转换、Node creator/Definition 双表 | RFC-0058 已完成统一 |
| `GetRawBatch` 和上一轮部分 Session 便利查询 | 已删除 |

[2026-09-14 审查](../archive/FRAMEWORK_SLIMMING_REVIEW_2026-09-14.md) 是历史基线。本文 C2–C4、D1 延续其尚未实施部分；不能直接按旧报告再次执行已完成的 A/B 项。历史 RFC 和验收记录保留原始语境，不改写成当前状态。

## 4. V1：先修默认测试装配漏选

**优先级最高；这是本次审查发现的覆盖缺口，不是应删除的测试。**

### 4.1 当前证据

[test_framework_core.cpp](../../tests/unit/core/test_framework_core.cpp) 第 120、135 行定义：

- `SessionContextTest.TypedDynamicResourcesRejectMismatchedAccess`。
- `SessionContextTest.SingleFlightCreatesOneTypedResource`。

但 [Tests.cmake](../../cmake_ext/Tests.cmake) 第 227–229 行的 `FrameworkCoreTest` 只选择 `AlgContextTest/TraceableItemTest/NodeRegistryTest/ModelManagerTest/PipelineTest`，没有 `SessionContextTest.*`。

同样，[test_adapter_purity.cpp](../../tests/unit/adapter/test_adapter_purity.cpp) 第 699 行起还定义 `RequestResultsTest`、`AdapterResultTest`、`ReadMultiWayResultsTest`、`OneToOneTextAdapterTest`；第 318–319 行 CTest 装配只选择 `AdapterPurityTest.*`。仅把源文件编译进 runner，不会自动执行这些套件。

已实际对四个现有 runner 执行 `--gtest_list_tests`，与 `ctest --show-only=json-v1` 对照：

| runner | 已编译 GoogleTest 用例 | 被 CTest 过滤器漏选 | 重复选择 |
| --- | ---: | ---: | ---: |
| `edgeflow_test_core_runner` | 209 | 3 | 0 |
| `edgeflow_test_adapter_runner` | 174 | 6 | 0 |
| `edgeflow_test_nodes_runner` | 296 | 0 | 0 |
| `edgeflow_test_tooling_runner` | 54 | 0 | 0 |
| 合计 | 733 | 9 | 0 |

9 个漏选用例均已存在于当前二进制，不能归因为源码尚未编译。完整遗漏清单：

| 所属 suite | 漏选用例 |
| --- | --- |
| `SessionContextTest` | `TypedDynamicResourcesRejectMismatchedAccess`、`SingleFlightCreatesOneTypedResource` |
| `NodeErrorCodesTest` | `UsesDistinctNodeDomains`（[test_node_base_contracts.cpp](../../tests/unit/core/test_node_base_contracts.cpp) 第 150 行） |
| `RequestResultsTest` | `MultiWayAlignmentAndAccessors` |
| `AdapterResultTest` | `TypedAndVoidMethods` |
| `ReadMultiWayResultsTest` | `ReadsAndAlignsMultiWayResults`、`ErrorMappingsAndDiagnostics`、`RejectsNonRequiredBindingsWithDiagnostic` |
| `OneToOneTextAdapterTest` | `CustomNullContextHooks` |

整体 CTest 清单有 97 个条目，其中 48 个引用这四个 runner。CTest 条目与 GoogleTest 用例是不同层级，不能把“97 个条目通过”解释为所有已编译用例都执行过。本次清单核对本身没有执行用例断言。

[TestInventory.cmake](../../cmake_ext/TestInventory.cmake) 的 required inventory 检查只证明 CTest 名称存在，不能发现该名称的过滤器遗漏了同文件新增套件。Individual 模式不按这些 suite 过滤，也不能作为默认模式已覆盖的证据。

### 4.2 实施动作

1. 在现有 CTest 分组补齐遗漏 suite：第 227 行 `FrameworkCoreTest` 加 `SessionContextTest.*`；第 248 行 `NodeBaseContractsTest` 加 `NodeErrorCodesTest.*`；第 318 行 `AdapterPurityTest` 加上表四个 Adapter suite。沿用 owning component 的 labels 和运行目录，不为每个遗漏 suite 新建 executable。
2. 对所有已构建测试可执行文件枚举 `--gtest_list_tests`，与 `ctest --show-only=json-v1` 中实际 `command` 的 GoogleTest 过滤器做集合核对。
3. 差集非空即报错；有意禁用、资产条件、独立冲突进程等例外应有明确理由。解析时处理参数化/类型化测试、正负过滤器、`DISABLED_` 和环境过滤，避免粗略按 suite 前缀猜测。
4. 复用现有 inventory/标签契约检查入口；检查实际已编译用例，不再维护第三份手抄 suite 名单。保留 CTest 级名称、标签、fixture、timeout 的原检查，它们负责另一层契约。
5. 聚焦执行此前漏选的套件；如果暴露失败，先定位修复，再做下文测试删除。不得为了门禁变绿重新从过滤器排除用例。

不必先完成 C4 的布局收口才能修此问题。用例清单中包含某测试只证明“会被选中”，实际断言通过仍须运行证明。

## 5. A 类：可以先做的局部减法

### A1. 删除没有读取的线程数状态和内部包装

| 位置与符号 | 证据 | 动作 |
| --- | --- | --- |
| [pipeline.h](../../include/core/pipeline.h) 第 102 行 `max_parallel_workers_`；[pipeline.cpp](../../src/core/pipeline.cpp) 第 472 行 | 仅声明和赋值，没有读取 | 删除成员和赋值；`RuntimeAssembly::max_parallel_workers` 也可由 `config.max_parallel_workers` 直接替代 |
| [result_writer.h](../../demo/common/result_writer.h) 第 26 行 `DemoRunSummary` | 全仓仅定义；实际汇总由 `result_writer.cpp` 第 152 行构造 JSON | 删除未使用类型及专属注释 |
| [scaffold_custom_node.py](../../scripts/scaffold_custom_node.py) 第 311、319、1005 行 | `render_compute_node`、`render_unary_inference_node`、`add_to_cmakelists` 全仓仅定义；CLI 使用 `render_node`、`updated_cmakelists` | 删除三个包装 |

保留 Pipeline 配置字段 `max_parallel_workers`、范围校验、实际线程池与日志；保留汇总 JSON 格式。`render_model_node` 被 `dev_recipe.py` 和测试调用，不能一起删。约几十行的收益主要是消除误导入口，不需要新增“证明某符号不存在”的测试。

验证：`PipelineConfigTest.ParallelModeWorkersBoundaries`、`DagPipelineTest.ParallelWavefrontExecution`、Demo 结果写入/追加，以及现有 scaffold/recipe 测试。风险低，通常无需 RFC。

### A2. 让 Validator 返回局部计划，避免整对象复制

**位置：** [pipeline_validator.cpp](../../src/core/pipeline_validator.cpp) 第 889 行 `finish_plan`，三个 `return finish_plan(plan)` 出口在第 900、1135、1491 行。

`finish_plan` 返回 `ValidatedPipelinePlan&`。外层按值返回时，表达式是函数调用产生的左值，不能对局部 `plan` 使用命名返回值优化或隐式移动。这会进入计划的复制语义，包括 JSON、节点/模型计划、报告和拓扑容器。

改成只更新 remediation 和 `report.ok` 的 `void` helper，各出口写：

```cpp
finish_plan(plan);
return plan;
```

不要改成 `return std::move(plan)`，以免阻止命名返回值优化。所有早退出口仍须完成诊断整理；无效图返回的部分规划也要保留。该项与 C2 的拓扑存储收口独立，先做即可。

验证：`ValidatedPipelinePlanTest.*` 和 `PipelineValidatorTest.*` 的错误码、路径、remediation、部分计划；无需镜像实现的测试。风险低，通常无需 RFC；不以本项推算未经测量的性能收益。

### A3. 删除已有控制流保证的 Core 回退

**位置：** [pipeline_validator.cpp](../../src/core/pipeline_validator.cpp) 第 1051–1103、1168–1173 行。

找不到 Definition 时已经 `continue`，后面的 `if (definition)` 是重复条件。对继续处理的节点，归一化配置必写入 `normalized_config_by_node`；第二轮已找到该节点 Definition 后再回退 `node.config`，没有正常可达路径。

实施：移除重复条件的嵌套；用 `normalized_config_by_node.at(id)` 表达必有值，删除原始配置回退。若顺便将归一化 JSON 改为 move，须在本轮模型依赖检查读取结束后移动。

保留默认值注入、字段错误、Definition callback 异常和归一化配置作为运行时唯一来源。验证 `ValidatedPipelinePlanTest.NormalizedNodeConfigIsRuntimeSingleSource` 及 Definition schema 默认值/异常测试。风险低，通常无需 RFC。

### A4. 删除 Adapter 来源校验后的不可能兜底

**依据：** [result_validation.h](../../include/adapter/result_validation.h) 第 13 行 `IndexResults` 会拒绝缺失的原始 ID 表指针、缺失/重复/越界来源；一对一模式还拒绝非零 `sub_id`。成功后，结果与原始 ID 表形成完整一对一对应。结果批次和 ID vector 同为空时可以成功，但输出循环不会执行。

以下 `PackTyped` 已成功调用它，随后仍回退到内部 `req_id`，使读者误以为缺少外部 ID 时也能输出：

- [keyword_match_adapter.cpp](../../src/adapter/biz/keyword_match_adapter.cpp) 第 117 行。
- [entity_extract_adapter.cpp](../../src/adapter/biz/entity_extract_adapter.cpp) 第 83 行。
- [audio_asr_intent_adapter.cpp](../../src/adapter/biz/audio_asr_intent_adapter.cpp) 第 149 行。
- [ocr_doc_qa_adapter.cpp](../../src/adapter/biz/ocr_doc_qa_adapter.cpp) 第 138 行。
- [compliance_audit_adapter.cpp](../../src/adapter/biz/compliance_audit_adapter.cpp) 第 154 行。
- [adapter_authoring.h](../../include/adapter/adapter_authoring.h) 第 166 行。
- [cross_rerank_adapter.cpp](../../src/adapter/biz/cross_rerank_adapter.cpp) 第 165、174 行；该处使用 ranked 模式，但原始 ID 表仍必需。

将已验证范围内的恢复 ID 简化为 `(*raw_req_ids)[i]`，CrossRerank 的输出数量直接取原始 ID 数。ASR 的 `intent_slots` 默认 `"{}"`、OCR 的 `box_count=0` 也位于相应批次已通过完整索引之后，可直接读取已对齐数据。

**边界：** 保留 `IndexResults` 本体与调用顺序；不得把“校验后的必有值”扩大成删除入口检查。`RequestResults::RequestId` 的公共构造/默认构造路径不同，不在本项机械删除范围。CrossRerank 的排名、容量和 `original_sub_id` 校验仍保留。

验证现有 Adapter purity 的乱序、重复、越界、缺字段、外部重复 ID，以及 C ABI/owned 输出两条路径。风险低至中；行为不变的局部重构无需 RFC，涉及输出失败语义的额外变化应另列。

## 6. B 类：通过少量共享代码减少理解负担

### B1. 收口函数式 Node 的模型绑定和错误映射

**位置：** [function_node.h](../../include/nodes/function_node.h) 第 633、704 行的 `LlmModelSlotBinding::Bind` 与 `EmbeddingModelSlotBinding::Bind`。

两者重复 plan 查找、槽位与 capability 校验、无 plan 时读取配置字符串、拒绝空模型 ID。抽同文件的小 helper（如 `ResolveBoundModelId`），具体 typed model 获取、`LlmCall`/`EmbeddingCall` 构造仍留在各自类中。

[model_calls.h](../../include/nodes/model_calls.h) 第 19、72 行也可共用“对齐结果转 `NodeResult`”辅助函数，保留 `Generate`/`Embed`、options 和业务错误说明。若 helper 比原两个短分支更难读，则仅实施绑定解析部分。

保留：plan 中缺绑定时不得回退原始 config；空输入提前成功；底层失败码、count/provenance 分类、`align` 阶段和槽名；成功验证后才移动输出。不要扩展成通用 Registry 框架，也不要顺带删除无 plan 初始化入口。

验证 [test_function_node.cpp](../../tests/unit/nodes/test_function_node.cpp) 中 `HarnessAllowsModelSlotsToShareOneModel`、`HarnessUsesDefinitionDefaultModelReferences`、`BindingValidationEnforcedInInitAndHarness` 及模型失败、options、对齐、重试；只补实际缺失的 embedding 分支。风险低，通常无需 RFC。

### B2. DocQA 结果读取公共组件只做一次前置校验

**位置：** [adapter_batch.h](../../include/adapter/adapter_batch.h) 第 237、332 行两种 `ReadMultiWayResults`。

带输出参数的重载先检查 ctx、spec、主结果，再检查容量，然后调用不带输出参数的重载；后者重新检查相同 ctx/spec/主结果。其后索引逻辑又重复从 Context 读取次要结果。

建议分成一个前置读取步骤与一个“使用已读取批次进行对齐”的私有步骤。带输出重载在两步之间做容量校验，核心重载直接接续第二步；不改变公开入口就能先去掉重复执行。是否缓存次要指针按净可读性决定，不为少数结果引入反射式框架。

**严格保持顺序：** ctx/spec/主结果 → 输出容量（带输出重载）→ 次要结果数量 → 原始 ID → 来源对齐。不能改成先运行完整核心重载再检查容量，否则多重非法输入时的诊断优先级变化。

验证 `ReadMultiWayResultsTest.*`、`AdapterPurityTest.DocQaAdapterDiagnosticsCharacterization`、乱序三路结果和容量预查询；先完成 V1，确保这些测试实际运行。风险中，行为不变通常无需 RFC。删除 `required` 选项属于 C1，不混入本次机械提取。

### B3. Demo 共享创建步骤；JSON launcher 只编码一次

**创建步骤：** [operator_runner.h](../../demo/common/operator_runner.h) 第 217 行与 [ocr_doc_qa_demo.cpp](../../demo/biz/ocr_doc_qa_demo.cpp) 第 45–91 行重复配置匹配、chip 解析、模型根路径、batch/depth、`CreateParam`、Create 与 Control 准备。

提取 Demo 私有、同步完成 Create 的 helper；调用方沿用 `OperatorHandleGuard`，Control 仍在 Process 前执行。保留 OCR 双输入槽与通用 runner 的单槽分批差异，不把 OCR 强塞进单槽模板。局部字符串在同步 Create 期间须有效，输出释放/handle 销毁顺序不变。若改动引入新所有权对象或异步 Create，则超出本项，须重新设计。

**请求编码：** [json_prompt_demo.py](../../demo/json_prompt_demo.py) 第 49、85 行，`main` 预先遍历 `encode_request`，`run_demo` 又编码一次。可在创建输出目录前一次生成规范化字符串列表，再交给私有执行 helper。直接 `run_demo` 调用仍须验证输入；CLI 避免两次 parse/dump。

保留完整 JSON 字段送入 SDK，Adapter 负责字段选择/响应组装；非法输入在 artifacts 产生前失败；结果数量、ID、重复和状态全部检查完才输出响应。验证现有 Demo Control-before-process、创建失败、chip 非法、结果保存以及 JSON launcher 批量/非法输入。风险低至中，通常无需 RFC。

## 7. T 类：测试删除与合并的精确清单

### T1. 可直接删掉的四个旧用例

“直接删”指保留目标覆盖并通过聚焦验证后删除，不是只看名字相近。

| 删除对象 | 保留对象 | 已核对的覆盖关系 |
| --- | --- | --- |
| [test_pipeline_catalog_validator.cpp](../../tests/integration/pipeline/test_pipeline_catalog_validator.cpp) 第 89 行 `BlackboardKeyTest.TypedOverloadsShareTheRuntimeKey` | [test_typed_blackboard_contracts.cpp](../../tests/unit/core/test_typed_blackboard_contracts.cpp) 第 20 行 `TypedBlackboardContractsTest.TypedKeyOperations` | 同样对 typed vector key 做 Publish/Has/Read 和完整值断言；保留项另测缺失值及 int。待删项并没有字符串重载交叉读取的独特覆盖 |
| 同 Validator 文件第 191 行 `PipelineValidatorTest.ReportsCycle` | 同文件第 267 行 `TableDrivenParityMatrix`；[invalid_pipeline_cases.json](../../tests/fixtures/pipelines/validation/invalid_pipeline_cases.json) 的 `dag_cycle` | 同类两节点互相依赖、同 code；矩阵另验证 Build/runtime parity |
| 同 Validator 文件第 208 行 `PipelineValidatorTest.ReportsDuplicateEdge` | 同矩阵的 `duplicate_dependency` | 同重复边刺激，已有 code 与 `/pipeline/1/depends_on/1` 断言 |
| [test_framework_core.cpp](../../tests/unit/core/test_framework_core.cpp) 第 161 行 `PipelineTest.ErrorHandlingAndRobustness` | [test_pipeline_config.cpp](../../tests/unit/core/test_pipeline_config.cpp) 第 1051 行 `MaterializationExceptionsAndFineGrainedDiagnostics`、第 1261 行 `OnceOnlyBuildContractAndStateMachineProtection` | 文件不存在的 code/path/Failed 与失败后再 Build 拒绝均已有更强断言。待删项第二次在已 Failed 的同一 Pipeline 上传 JSON，只测试状态机拒绝，没有测试注释所称畸形 JSON |

同步移除已空的 `BlackboardKeyTest.*` 等 filter；只有对应 suite 确实为空时才删其映射。保留其他 `PipelineTest` 用例。不得删除配置矩阵的“Model/Backend/Node 创建加载初始化次数均为零”断言，parity 矩阵不替代它。

### T2. 六节点注册冒烟：迁移两个独特断言后再删

待删：[test_framework_core.cpp](../../tests/unit/core/test_framework_core.cpp) 第 71 行 `NodeRegistryTest.DynamicReflection`。

保留落点：[test_catalog_contract_ssot.cpp](../../tests/contract/catalog/test_catalog_contract_ssot.cpp) 第 23 行 `AllProductionNodesHaveValidDefinitions` 已遍历所有生产 Definition 并检查 Has/Create/Find，覆盖旧测试硬编码六节点。

先迁移旧测试独有的：

1. `TextChunkNode` 实例 `Name()` 的断言。仅迁这个具体契约，不未经设计就要求所有 custom Node 的 `Name()` 等于类型名。
2. 不存在类型调用 `NodeRegistry::Create` 返回空；当前 `FindReturnsEmptyForNonexistentEntities` 只测 Find，不足以替代 Create。

迁移后删除旧测试及空 suite filter。不新增 executable，通常无需 RFC。

### T3. 旧 DAG 格式负例归入配置矩阵

[test_dag_pipeline.cpp](../../tests/unit/core/test_dag_pipeline.cpp) 第 458 行 `RejectsLegacyPipelineWithoutIdOrDependsOn` 测格式拒绝，适合归入 [test_pipeline_config.cpp](../../tests/unit/core/test_pipeline_config.cpp) 的现有 Build 负例矩阵。

先增加“同时缺 id 和 depends_on”行，保留首诊断及零副作用断言，再删 DAG suite 独立用例。保留直接 parser 的 `RejectsPipelineWithoutIdOrDependsOn`，以及矩阵单独缺 id/depends_on 两个边界。此项是归属整理，不是放弃对旧输入的拒绝。

### T4. FixedBatchExecutor 用例迁移，不删除异常覆盖

[test_model_backend_decoupling.cpp](../../tests/unit/engine/test_model_backend_decoupling.cpp) 第 602 行 `FixedBatchExecutorStrictOutputsAndRollback` 是执行器测试，应迁到已有 [test_batch_executor.cpp](../../tests/unit/engine/test_batch_executor.cpp)。

现有专门 suite 只覆盖四类正常/基础错误场景，没有替代该用例的输出数量多/少、抛异常回滚。迁移后按场景拆分或表驱动，保留动态策略 `{2,0}`、输出清空、数量错误 `-3`、异常 `-4`；如缺少第二批才失败的场景，可补以证明第一批已成功输出也会回滚。修改 suite 名时同步 filter，仍使用现有 runner。

### T5. Quality gate 自测统一测试语言

[test_quality_gate_contract.sh](../../tests/contract/architecture/test_quality_gate_contract.sh) 第 18–86 行创建假 cmake/ctest/ccache 并测试 sanitizer；随后调用 [test_quality_gate_contract.py](../../tests/contract/architecture/test_quality_gate_contract.py)，后者又构造临时仓库和假命令。

将 shell 的 sanitizer 参数、日志和 ccache 断言迁进 Python 独立检查函数，复用临时目录和结构化参数日志；CTest 直接调用 Python，再删 shell 文件。

两个文件的断言并不完全重复。必须保留 C/C++ launcher、PCH、调用者 `CCACHE_DIR`、zero/show stats、BUILD_TESTING、空测试集合失败、失败传播、缓存翻转和交付证据断言。收益是减少两套测试基础设施，不是整批删除 shell 中的 sanitizer 测试。

### T6. 去掉漂移自测中的当前版本硬编码

[test_architecture_docs_drift_gate.sh](../../tests/contract/architecture/test_architecture_docs_drift_gate.sh) 第 56 行固定替换 `10.0.0` 为 `99.0.0`，升级后可能不再真正污染 fixture。

从当前 CMake 版本声明取得值，断言原文存在、替换确实改变内容，再证明 gate 拒绝。保留该负向测试；本项是修正刺激的可靠性，不是删掉文档治理。无需新增 suite。

## 8. C 类：现在适合决策，但须明确契约迁移

以下建议会改变可调用 C++ 接口、诊断或开发模式，按 CONTRIBUTING 先写 RFC，再迁消费者、删旧路径。这里的 RFC 是实施记录要求，不需要把普通局部修复暂停等待额外审批。

### C1. 删除 Adapter 为历史行为和未实现选项保留的分支

这是本次仍能明确定位到的历史兼容负担。

**证据：** [adapter_authoring.h](../../include/adapter/adapter_authoring.h) 第 54–60、85–130 行；[translate_adapter.cpp](../../src/adapter/biz/translate_adapter.cpp) 第 55–56 行；[RFC-0053 §2.3](../rfcs/0053-function-oriented-adapter-authoring.md) 明确要求当时保留诊断不一致。

`ResultBindingSpec` 与 `ValidateRequiredSpecs` 位于 [adapter_batch.h](../../include/adapter/adapter_batch.h) 第 133、168 行。

| 当前设计 | 为何可收口 | 建议目标 |
| --- | --- | --- |
| `carrier_adapter_name="EntityExtract"`，Translate 部分错误标为 EntityExtract | 载体复用却保留旧业务名 | 诊断统一使用实际 AdapterName；不改变底层 C 载体和完整 JSON 请求语义 |
| Translate 缺 answers/IDs 返回 INVALID_INPUT，但 `AdapterStatus.Code` 是 BUFFER_TOO_SMALL | 两种错误身份需要作者记忆例外，代码注释明称兼容约定 | 本 Spec 统一返回码与状态码为 INVALID_INPUT；局部使用合适状态 helper，不全局改所有 Adapter 的错误映射 |
| 空 ctx 字段写 `json`，存在两种自定义 null-context hook | 生产 Spec 仅 Translate 使用特例；hooks 只有直接作者接口测试消费 | 将本 Spec 的空 ctx 统一为 INVALID_INPUT、字段 `ctx`，删除 hooks 和 `null_ctx_field`；其他旧 Adapter 不在本项批量改码 |
| `ResultBindingSpec.required=false` 只能被 `ValidateRequiredSpecs` 拒绝，根本不支持 optional | 暴露一个没有成功执行语义的选项，并在两个重载重复检查 | 当前组件固定 required，删除 bool/构造参数及专属拒绝路径；未来需要 optional 时再设计具有真实语义的独立类型 |

这里**不是零引用删除**：[test_adapter_purity.cpp](../../tests/unit/adapter/test_adapter_purity.cpp) 第 885、1005 行测试 optional 拒绝和自定义 hooks，[test_adapter_contract_security.cpp](../../tests/contract/abi/test_adapter_contract_security.cpp) 第 871 行专门测试 `TranslateNullContextLegacyDiagnostics`。先确定新契约，再迁移/删除专属旧断言。

实施顺序：

1. 新 RFC 明确取代 RFC-0053 中诊断来源和兼容绑定的指定部分；记录新旧映射表。保留历史 RFC 原文。
2. 写新契约的最小行为测试，迁 Translate 与 DocQA 的 Spec 构造；再删除字段和分支。同步删除 `UnpackTextBatchSkeleton` 在 `adapter_batch.h` 第 38、46–48 行的 `carrier_adapter_name` 参数与选择逻辑；其唯一显式生产传值源就是该 Spec，避免留下另一套诊断身份入口。
3. `CustomNullContextHooks`、`RejectsNonRequiredBindingsWithDiagnostic` 在接口删除后可删；`TranslateNullContextLegacyDiagnostics` 改为新空 ctx 契约测试，不删除空指针防护。
4. 检查 `Pack` 与 `PackResultBatch` 的一致性；通过 C ABI 触发相关可达错误，确认返回码和错误文本不再混用 Adapter 身份。

**不顺带改变：** 全批 carrier 验证先于业务 Decode；Translate 全批序列化先于容量/来源检查；非法 UTF-8 的异常路径；Entity 逐字段写入与失败时部分修改；容量预查询和外部 ID 恢复。删除这些历史保持点是另一个更大的行为决策，本轮收益不足以支持一并改变。

另可修正 `UnpackTextBatchSkeleton` 注释中的“原子发布”：实际是全部业务转换成功后逐 key 发布，Context 没有多 key 事务；RFC-0053 §3.1 已明确这一点。应修注释，不因用词不准另建事务系统。

### C2. 执行计划拓扑只保留一份

[pipeline_validator.h](../../include/core/pipeline_validator.h) 第 59–60、82–83 行，`ValidationReport` 与 `ValidatedPipelinePlan` 都持有 `topological_order/layers`；[pipeline_validator.cpp](../../src/core/pipeline_validator.cpp) 第 1129–1130 行复制两份容器。

推荐保留 report 中的唯一存储，删除 plan 外层重复字段；Pipeline 执行与查询读取 `plan_->report.topological_*`。不新增共享指针或引用成员来维持两套字段外观。

RFC 明确一次性迁移可见 C++ 字段；保持 `ValidationReport::ToJson()` 的 `plan.topological_order/layers` JSON 形状、无效图部分计划和 Pipeline 不重排。验证多层/菱形/乱序 DAG、错误实例标识与 Control 路由。

除 Pipeline 消费外，同批迁移 [test_text_embedding_node.cpp](../../tests/unit/nodes/test_text_embedding_node.cpp) 第 445 行、[test_onnx_and_reranker_model.cpp](../../tests/unit/engine/test_onnx_and_reranker_model.cpp) 第 958 行及 [test_validated_pipeline_plan.cpp](../../tests/unit/core/test_validated_pipeline_plan.cpp) 第 402、456 行等直接访问计划外层拓扑的测试；保留它们的行为断言。

`nodes_` 的所有权和 `node_layers_` 的执行索引职责不同，不属于这项冗余。风险中；A2 的按值返回优化可先做。

### C3. 统一严格验证，删除测试依赖的宽松策略

[pipeline_validator.h](../../include/core/pipeline_validator.h) 第 17 行定义 `kStrict/kPrivateExtensionCompatible`；[pipeline_validator.cpp](../../src/core/pipeline_validator.cpp) 第 904、931、940、1133 行按策略放宽未知 biz/缺 Definition 等条件。仓内显式宽松调用在测试与 Harness；生产源码没有显式选择它。

它是 [RFC-0008](../rfcs/0008-architecture-contract-consolidation.md) 保留的私有扩展契约，仍有测试消费者，不能直接归为死代码。**建议目标是统一严格验证**，减少一种可执行合同。

迁移：先给测试注册最小且完整的 biz ingress/egress 和 Definition；再把 Harness 与直接调用迁到 strict；最后删除策略枚举/参数/条件。将 `StrictVsCompatiblePolicy` 改为未知 biz 拒绝、完整注册通过和 I/O 闭包校验。

范围还包括 [pipeline.h](../../include/core/pipeline.h) 第 51、54、95 行及 [pipeline.cpp](../../src/core/pipeline.cpp) 第 304、359、425 行的 policy 透传，不能仅删 Validator 入口。[node_harness.h](../../tests/support/node_harness.h) 第 214 行固定使用 `harness_biz`，却按不同 Node 在第 272–280 行生成不同端口；RFC 必须明确测试 biz 契约的生成/注册和同名冲突处理，不能注册一次空 `harness_biz` 了事。

不要仅将枚举替换成 strict 后修改期望“让测试通过”，也不要用空 biz 定义绕过闭包。Registry `Has` 为真不代表复制 Definition 的 `Find` 必定成功，失败仍要关闭。此项跨测试面较大，风险中高，单独实施。

### C4. 默认保留分组 runner，取消整套 Individual 布局

[Tests.cmake](../../cmake_ext/Tests.cmake) 第 17–23 行的模式分流与 [IndividualTests.cmake](../../cmake_ext/IndividualTests.cmake) 维护两种装配。source inventory 已共享，但 filters、labels、部分 fixture/编译配置和 timeout 不完全共享；V1 已显示默认模式可漏掉同文件新增套件。

当前 CI/gate/sanitizer/preset 使用分组 runner。Individual 模式仍在 `tests/README.md` 和 scaffold 的构建目标建议中使用，属于开发功能，不是无人消费的文件。

**推荐决策：** 统一“分组 runner + `--gtest_filter`”作为普通诊断方式，下线整套按文件生成 executable 的模式。

需要同批处理：CMake 选项和分支、IndividualTests.cmake、scaffold 的模式探测和 OFF 专项测试、preset/脚本/指南说明。先核对所有源、链接依赖、generated fixtures、编译定义、labels、timeout 均由保留布局覆盖。

保留 Registry 启动冲突、特定分配替换等**确需独立进程**的目标；取消 Individual 模式不等于把所有测试放进一个进程。保留 V1 的实际用例集合校验，单一布局也可能写错 filter。

若团队仍需要整套逐文件模式，则替代方案是由同一声明生成两种布局；不要继续手工维护两套测试元数据。本计划不同时实施这两种相反方案。风险中，需 RFC 记录开发工作流变化。

## 9. D 类：暂缓或顺手评估

### D1. `IModel::GetMaxBatchSize` 可缩小接口，但不是本轮首选

[model_interface.h](../../include/engine/model_interface.h) 第 26 行强制所有 Model 实现查询；生产 `src/` 中只有定义，没有查询消费者。但 ASR/OCR test doubles 用它构造 `BatchPolicy`，测试和 benchmark 也实现/断言它。

若收口：先 RFC 删除统一必实现接口，模型内部保留真正需要的限额；测试改用局部常量或私有 helper；迁所有 Model/Mock/benchmark。保留 Backend batch policy 和 `FixedBatchExecutor::Execute`，证明 padding、动态尾批、数量错误和 provenance 不变。不把“无生产调用”当成可以取消批次约束的依据。

### D2. BGE 张量行复制：净收益为正才提取

[bge_embedding_model.cpp](../../src/engine/models/bge_embedding/bge_embedding_model.cpp) 第 266 行起与 [bge_reranker_model.cpp](../../src/engine/models/bge_reranker/bge_reranker_model.cpp) 第 216 行起重复向 ids/mask/type 张量复制一行。可在已有 `BertInputTensors` 私有支持中提供写行方法。

保留单文本/成对编码、真实 segment IDs、可选 token types、最小长度 2/3 差异和 pooling 所用 mask 生命周期。若新增 helper 的参数和校验比复制逻辑更难读，则跳过；无需为几十行复制引入更大模板抽象。涉及真实 ONNX 行为时，资产缺失的 skip 不能当作完成验证。

### D3. 其他开发接口先保留

- `TraceableUnaryInferenceNode` 仍被高级 scaffold 生成并有契约测试；生产 Node 暂无派生不构成删除理由。
- Session 的类型检查、single-flight、revision/cache 机制承担资源和并发契约；本次没有足以支持删除的证据。
- `run_all_demos.sh` 是薄 CLI，但提供默认 smoke、工作目录和二进制检查。无代码调用不代表没有用户价值，不默认删除。
- `ResultWriter::WriteResults(total_duration_ms)` 虽生产 Demo 均传 `0.0`，测试保护非零覆盖语义；若要删除，应先确定 summary 耗时定义。
- `alg_show` 与 Studio 的终端展示用途不同；浏览未完成配置不应被强制执行 Validator。
- RFC-0052/0054/0057 在实施中的作者接口、快照和编排工作，不另开相冲突的重写路线。

## 10. 不应按冗余删除的代码与测试

1. **六个 `Alg_*` 的 C11/noexcept/双 catch 屏障、输入 copy-in、输出容量与来源校验。** C ABI 和直接 Adapter 调用是不同边界，Runtime 预检不能替代直接调用防护。
2. **完整 Adapter 转换、Operator bridge/ValueType 注册、输出池与事务发布。** C 固定数组和 owned 变长结果有不同容量/生命周期；不能合并成先转固定 C DTO，也不能交给 Demo 组装业务响应。
3. **失败 Build 的临时组装、状态 guard、并行等待 guard。** 它们保护失败原子性和请求结束时的线程状态。
4. **Model/Backend 分离、厂商生命周期、FixedBatch padding/回滚/provenance。** 未找到把不同 Backend 的实现当旧版本兼容删掉的依据。
5. **拒绝旧配置的负例。** `Legacy` 名称不意味着支持旧格式；可整理名字和位置，仍需证明旧字段/缺显式 DAG 被拒绝。
6. **缓存缺必需元数据的测试。** `test_llama_cache.cmake` 去掉 BLAS 元数据、保留 archive 并重算合法 hash，证明内容不完整的缓存被拒绝；其他缓存损坏用例不替代它。
7. **治理 gate 与其污染 fixture 的自测。** 一个检查项目现状，一个证明检查器能发现问题；不能视为重复。
8. **RFC-0058 的重入、原子注册、分配失败、诊断 parity，以及配置零副作用测试。** 这些测试覆盖不同失败边界，不能只因最终都返回失败而合并删除。
9. **`edgeflow/operator/types.h`。** 当前规范门面隔离 `platform_mock` 位置；注释含 Compatibility 不足以证明它是上一轮遗漏的旧头。

## 11. 实施与验收步骤

### 11.1 每批工作方式

1. 从届时基线创建 `refactor/*` 或 `test/*` 分支，先核对本文候选仍存在，保留其他人的改动。
2. 契约变化先记录 RFC，明确取代范围；局部精简不用额外 RFC。
3. 测试删除先列“刺激、边界、断言、保留位置”，迁独特断言，再删旧用例和空 filter。生成器、fixtures、文档同批更新。
4. 在所属现有 runner 做聚焦验证；涉及 public ABI、Core 契约、所有权或并发的变化做独立复核。
5. 每个准备交付的批次完成后只跑一次 canonical gate：`./scripts/run_all_tests.sh`。失败或再次修改后按流程重验，不在门禁前后例行重复完整构建/CTest。
6. 用户可见或架构变化更新 CHANGELOG；纯内部机械减法无需每项加记录。完成项在本计划标记并链接实际 RFC/结果；全部结束后将计划归档。

### 11.2 聚焦命令示例

下列是实施时的命令示例，不表示本文已经执行这些修改或验收。

```bash
# V1：列实际用例及 CTest 命令，再执行遗漏 suite
./build/edgeflow_test_core_runner --gtest_list_tests
ctest --test-dir build --show-only=json-v1
./build/edgeflow_test_core_runner --gtest_filter='SessionContextTest.*:NodeErrorCodesTest.*'
./build/edgeflow_test_adapter_runner --gtest_filter='RequestResultsTest.*:AdapterResultTest.*:ReadMultiWayResultsTest.*:OneToOneTextAdapterTest.*'

# Core / 测试精简
cmake --build build --target edgeflow_test_core_runner edgeflow_test_tooling_runner -j 4
ctest --test-dir build -R '^(FrameworkCoreTest|PipelineConfigTest|ValidatedPipelinePlanTest|TypedBlackboardContractsTest|PipelineStudioTest)$' --output-on-failure

# Integration：需先修 V1 的 filters
cmake --build build --target edgeflow_test_adapter_runner -j 4
ctest --test-dir build -R '^(AdapterPurityTest|AdapterContractSecurityTest|CAbiSafetyTest|OperatorApiTest)$' --output-on-failure

# 开发基础设施：按实际修改挑选，而非每次全部运行
ctest --test-dir build -R '^(QualityGateScriptsContractTest|ArchitectureDocsDriftGateSelfTest)$' --output-on-failure
```

如果本批改注册/Definition/配置，重建目标工具，查询 Catalog，对受影响 Pipeline 执行 `validate`/`plan` 并运行对应业务路径；测试专用注册用 `alg_pipeline_tool_test`。ABI 变化要直接覆盖完整 C ABI 请求/响应，不以 Demo 成功替代。

### 11.3 验收表

| 维度 | 必须证明 |
| --- | --- |
| 删除确实完成 | 旧符号无非历史消费者；没有别名、双写或自动回退重新引入 |
| 代码容易理解 | 规则位于明确所有者；helper 的参数/间接层没有抵消精简收益 |
| 测试集合完整 | 实际已编译用例与 CTest 过滤器匹配；例外明确，漏选为零 |
| 断言没有丢失 | 待删用例独特刺激/断言有明确保留位置；参数化仍能定位失败场景 |
| 当前外部行为稳定 | 无契约变化的批次保持错误码、诊断、输出、来源和失败顺序；有变化的按 RFC 新表验证 |
| 构建范围清楚 | 默认 gate 通过；非默认 Backend/资产/硬件限制单独报告，不能用 mock 替代 |

## 12. 实施与验证交付记录

### 12.1 批次 0、1、2 实施总结（2026-09-15）

已完成批次 0（V1）、批次 1（A1–A4、T1–T3、T6）和批次 2（B1–B3、T4–T5）全部项目实施与验证：

- **V1（测试装配与编译用例全量映射）：** 补全 `FrameworkCoreTest`、`NodeBaseContractsTest`、`AdapterPurityTest` 遗漏 suite；`test_test_labels_contract.py` 动态核对已编译用例与 CTest 过滤器的差集，排除 `.so` 动态库并强化 suite 标题解析与连字符分割，保证 100% 覆盖。
- **A1–A4（局部精简）：** 删除 `pipeline.h/.cpp` 未读成员 `max_parallel_workers_`；删除 `result_writer.h` 未使用类型 `DemoRunSummary`；删除 `scaffold_custom_node.py` 未使用包装；Validator 改用 `void finish_plan(plan)` 配合 NRVO；移除冗余 `if (definition)` 检查，统一以 `normalized_config_by_node.at(id)` 访问；清理 7 处 Adapter 来源校验后不可能可达的内部 `req_id` 回退。
- **B1–B3（共享实现收口）：** 提取 `function_node.h` 中的 `ResolveBoundModelId`；提取 `model_calls.h` 的 `ConvertAlignedOutputs`；`adapter_batch.h` 拆分前置校验 `ValidatePrimaryAndSpecs` 与对齐 `AlignAndIndexResults`，严格保持校验诊断优先级；提取 `operator_runner.h` 的 `CreateOperatorInstance` 并在 `ocr_doc_qa_demo.cpp` 复用；`json_prompt_demo.py` 提供单次规范化 `prepare_requests` 并由私有 `_run_demo_impl` 执行，同时保持公开 `run_demo` 入口自验证。
- **T1–T6（测试重整与归属收口）：** 删除 4 个确认重复用例并同步移除空 filter；将 `NodeRegistryTest` 独有断言迁至 `test_catalog_contract_ssot.cpp`；DAG 格式负例迁入 `test_pipeline_config.cpp`；`FixedBatchExecutorStrictOutputsAndRollback` 迁至 `test_batch_executor.cpp`；`test_quality_gate_contract.sh` 统一合并至 Python 契约测试；`test_architecture_docs_drift_gate.sh` 动态解析 CMakeLists.txt 版本号。
- **批次 3（C1–C4）：** 涉及公共契约收口与架构语义变更，保留作为后续独立 RFC 实施方案，不混入本次非破坏性精简。

### 12.2 验证证据

- `./scripts/run_all_tests.sh` 门禁 100% 通过（97/97 CTest 自动化测试，744 个唯一编译 GoogleTest 用例零遗漏覆盖）。
- 代码规范与静态检查全部通过（Shell 语法检查、Google clang-format、Git diff check）。

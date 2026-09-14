# 投产前框架冗余、兼容负担与瘦身审查

- 审查日期：2026-09-14
- 源码基线：`b08c5bd80e8422e8e5f6f44424e299ccd10f3d46`
- 前提：框架尚未正式投入生产，可以一次性迁移仓内消费者并放弃旧接口、旧配置兼容。
- 本次交付：审查报告；未实施下述删除或重构。建议不代表已经批准的架构决策。
- 代码行号对应上述基线。本文属于日期型审计，后续实现另行记录，不回写历史结论。

## 1. 结论

**存在明确的兼容残留，也存在可以收敛的重复机制，当前适合清理。主要收益是减少接口、配置语义和维护分支；现有证据不支持承诺显著降低 SDK 体积或提高推理性能。**

最值得先做的是：删除旧名称和无效果参数，统一模板语法，统一 Operator 输出配置及回调形式，删除 Studio 的旧 Catalog 消费分支。随后处理 Core 诊断码往返转换、Node 注册状态分散，以及没有生产消费者的查询和便利接口。

框架已经完成不少有效收敛：C ABI 与 Operator 共用内部运行时；Pipeline 使用同一个 Validator 和执行计划；Studio 委托 native Validator；Model 与 Backend 职责分离。建议采用有边界的减法，不建议重写四层架构，也不建议仅按文件长度、测试数量或抽象类数量决定删除。

这里讨论的兼容负担主要是**新版继续接受旧源码名称、旧配置和旧语法的向后兼容**，而不是要求旧版本理解未来新格式。

## 2. 范围、方法与证据限制

审查覆盖接入适配层 / Integration、流程编排层 / Orchestration、能力节点层 / Capability Nodes、模型执行层 / Model Execution，以及 Demo、Studio、脚本、CMake、测试、配置和治理文档。按层并行检查后交叉核对调用关系、生成器、测试依赖和已有 RFC；这不是每个函数均已形式化验证的正确性审计。

基于 `git ls-files` 的范围盘点如下。行数按所列目录内常见代码、配置、Markdown 和文本后缀统计，含注释与空行，排除生成物和第三方缓存，**不是可删除行数**。

| 范围 | 受版本管理文件数 | 文本行数 |
| --- | ---: | ---: |
| `src/` | 104 | 21,686 |
| `include/` | 84 | 12,163 |
| `tests/` | 95 | 39,091 |
| `demo/` | 38 | 4,735 |
| `tools/` | 12 | 3,503 |
| `scripts/` | 19 | 4,123 |
| `cmake_ext/` | 18 | 2,108 |
| `dev_support/` | 26 | 2,750 |
| `configs/` | 35 | 2,789 |
| `doc/` | 113 | 29,712 |

已实际运行 `./build/alg_pipeline_tool catalog`。现有构建返回 Catalog v3、12 个 Node、6 个 Model、8 个 biz、14 个 Profile，启用的 Backend 为 `llama_cpp` 和 `onnxruntime`。构建缓存为 Release，Kite/Whisper Backend 关闭；Catalog 中出现 Model 不等于当前构建具有可运行它的 Backend 或模型资产。首次查询使用已有二进制，不能单凭它证明所有源码路径均已重新构建。

对受版本管理、超过 100 字节的 `.cpp/.h/.hpp/.c/.py/.js/.sh/.cmake/.json/.conf` 文件做内容哈希比对，未发现整文件完全重复组。下文发现的主要是**同一职责的双表示、兼容分流和闲置接口**；整文件哈希无法排除这种逻辑重复。

“无调用”均指本基线仓库内符号检索和相关调用链检查，不包含无法访问的仓外扩展。未接入公司内部 SDK，未做目标硬件推理、真实模型效果验收或删除前后性能测量。

## 3. 建议总表

优先级表示实施顺序：P1 为优先清理，P2 为第二阶段设计收敛，P3 为需要产品范围决策。它不是安全漏洞严重性；风险表示删除或迁移的风险。

| 编号 | 建议 | 类型 | 优先级 / 风险 | 主要收益 |
| --- | --- | --- | --- | --- |
| A1 | 删除七个旧公共头入口及 NodeFactory 别名 | 明确历史兼容 | P1 / 低 | 唯一命名，减少公开入口 |
| A2 | 模板只保留共享解析器和一种占位符语法 | 明确历史兼容 | P1 / 中 | 消除多套语义和歧义分支 |
| A3 | Operator 输出配置统一为 data.outputs | 明确历史兼容 | P1 / 中 | 单/多输出统一解析 |
| A4 | Bridge 回调、标识和输出 key 单路径化 | 兼容及隐式默认 | P1 / 中 | 减少注册与执行分流 |
| A5 | 删除 --no-default-control | 无效果兼容参数 | P1 / 低 | 去除参数、冲突判断和内部传递 |
| A6 | Studio 只消费 Catalog v3 当前字段 | 旧格式消费分支 | P1 / 低 | 前端模型依赖单一表示 |
| B1 | Core 统一诊断码，保留必要报告容器 | 结构重复 | P2 / 中 | 删除往返和有损映射 |
| B2 | Node creator 与 Definition 统一存储 | 状态和所有权重复 | P2 / 高 | 减少双表一致性及锁约束 |
| B3 | 清理无消费者的 Model 查询及 Node 内部接口 | 接口范围过宽 | P2 / 低至中 | 减少必实现方法和裸指针暴露 |
| B4 | 清理 Session 便利查询及测试专用变更入口 | 接口范围过宽 | P2 / 低至中 | 缩小会话资源变更面 |
| C1 | 决定是否取消私有扩展宽松校验策略 | 历史策略及测试便利 | P3 / 中 | Validator 只维护一种完整契约 |
| C2 | 收敛 Node 初始化和脚手架作者路径 | 真实双路径 | P3 / 中至高 | 减少绑定和作者模式重复 |
| C3 | 收敛双套测试装配 | 开发设施重复 | P3 / 中 | 减少构建维护矩阵 |
| C4 | 合并静态规则、删工具别名及猜测式测试 | 工具和治理重复 | P2 / 低至中 | 减少规则漂移和测试兜底 |

## 4. P1：明确兼容残留

### A1. 删除旧公共头和旧注册表名称

**事实。** 七个旧入口仅转发当前头：`include/company_alg_interface.h`、`company_alg_cpp.hpp`、`company_alg_export.h`、`company_alg_log.h`、`company_alg_version.h`，以及 `include/operator/operator_interface.h`、`company_operator_types.h`。例如 [旧 C ABI 入口](../../include/company_alg_interface.h) 第 3 行明确标注 Compatibility。它们仍列在 [公共头清单](../../cmake_ext/LayerHeaderViews.cmake) 第 45–48 行。[NodeRegistry](../../include/core/node_registry.h) 第 139–140 行也明确保留 `NodeFactory` 源码兼容别名；当前 C++ 引用只有该声明。

**建议。** 仓内调用统一使用当前入口，然后删除七个旧头及 `NodeFactory`。同步清理公共头清单、依赖归属表和旧头成功编译测试。根 [CMakeLists.txt](../../CMakeLists.txt) 第 57–59 行为旧生成版本头维护的构建目录清理，可在要求一次干净构建后去掉。

**影响与验收。** 破坏旧 include 和旧 C++ 名称，不要求修改六个 C ABI 函数、结构布局或共享库名称。保留新头的 C11 编译与公共/扩展/内部头可见性测试。收益主要是入口数量，转发头本身几乎没有运行时成本。

**明确排除。** [edgeflow/operator/types.h](../../include/edgeflow/operator/types.h) 虽然也是转发头，却是 [源码布局指南](../dev_guide/source_layout.md) 第 105 行规定的当前路径，也是 `edgeflow/operator/interface.h` 的依赖。它可以隔离 `platform_mock/` 实现位置，不应与七个旧名称入口一起删除。未来如要改变此门面，应另作公开源码路径决策。

### A2. 删除 PromptGuided 的 legacy/auto，并统一占位符拼写

**事实。** [prompt_guided_llm_node.cpp](../../src/custom_nodes/prompt_guided_llm_node.cpp) 第 29–70 行维护独立旧模板解析器，91–106 行选择 `auto/standard/legacy` 并处理歧义，153–161 行公开三模式配置。全仓 JSON 搜索仅发现两份内置 PromptGuided 配置，均显式为 `standard`：[DocQA fixture](../../demo/fixtures/mock/pipeline_doc_qa_custom.json) 第 90 行和 [EntityExtract fixture](../../demo/fixtures/mock/pipeline_entity_extract_custom.json) 第 32 行。

共享 [text_template.h](../../include/nodes/text_template.h) 第 27 行又声明同时接受 `{{name}}` 与兼容的 `{name}`，第 50–72 行分别处理两种括号。这是另一层兼容，不会随着删除 PromptGuided 私有解析器自动消失。

**建议。** 固定调用共享解析器，移除 `template_syntax` 配置字段和私有旧解析器；将占位符统一为 `{{name}}`，普通 JSON 单花括号作为字面内容。一起迁移默认模板、两份 fixture、文档及模板 Control 示例。

**影响与验收。** 旧模板测试仍依赖旧语义，不能称无人使用。更新 [test_common_nodes.cpp](../../tests/unit/nodes/test_common_nodes.cpp) 第 1053、1183、1298 行附近，以及 [test_text_template_node.cpp](../../tests/unit/nodes/test_text_template_node.cpp) 第 82、103 行附近测试。保留未知变量、缺失 context 绑定、非法双括号、JSON 字面文本和 Control 更新校验，尤其保留“插入用户文本后不再二次解析”的行为。这里减少的是模板语义数量，不是提示词能力。

### A3. Operator 输出配置只保留 data.outputs

**事实。** [operator_config_resolver.cpp](../../src/adapter/operator/operator_config_resolver.cpp) 第 468 行显式区分 `legacy_output` / `named_outputs`；470 行检查互斥，479 行为旧格式限制单槽，514 行选择对应读取路径。

**不是死代码。** `configs/` 与 `demo/fixtures/` 下 25 份 `.conf` 使用 `mem_que`，未发现使用 `outputs` 的 `.conf`。多输出已有 [Operator 集成测试](../../tests/integration/operator/test_operator_api.cpp) 第 2447 行起的实际覆盖，第 2511 行起检查新旧格式不能混用。

**建议。** 单输出也统一写入以逻辑槽位为键的 `data.outputs`，一次迁移所有仓内配置、配置生成工具、Demo/Studio 草稿运行路径及测试；随后删除 `mem_que` 成功解析路径和双模式互斥分支。不要为此次迁移再增加常驻兼容开关或运行时转换器。

**影响与验收。** 配置契约破坏性变更，应立 RFC；所有官方 `.conf` 需重新 resolve，相关方案需 validate/plan 和 Demo smoke。保留未知输出槽、缺必需槽、容量、分配参数及总内存预算校验。不能只删除解析分支，让 25 份现有配置失效。

### A4. Bridge 描述符使用一套回调与标识规则

| 现状与证据 | 建议 |
| --- | --- |
| [operator_biz_bridge.h](../../include/adapter/operator_biz_bridge.h) 第 63 行有槽位 convert_output，第 96 行又有顶层 convert_sample_output；[registry](../../src/adapter/operator/operator_biz_bridge_registry.cpp) 第 141 行允许单输出回退；[执行入口](../../src/adapter/operator/operator_adapter.cpp) 第 398 行每次选择其一 | 所有输出回调下沉到槽位，删除顶层回调及比较、校验、执行回退；单槽 helper 保留并生成同一种描述符 |
| registry 第 256–270 行让 adapter_name 同时接受 AdapterName 和 Pipeline biz_name，注释明确称兼容旧 bridge | 只接受 AdapterName；内置桥已使用 DocQA、Translate 等 Adapter 标识，没有找到依赖此回退的内置桥 |
| header 第 61–66 行允许空 key_suffix 回退为 type_suffix，单槽 helper 第 129 行依赖该默认 | 可让 helper 显式填写同样的输出 key，注册时要求非空，删除运行时隐式回退；作为同一改动的低收益尾项 |

**影响与验收。** 改变 C++ 扩展描述符，迁移内置桥和 [registry 测试](../../tests/unit/operator/test_operator_biz_bridge_registry.cpp) 第 55、315、675 行附近的双形式用例。保留单/多输出端到端结果，以及分配失败、回收、注册冲突和缺回调拒绝测试。

**边界。** type_suffix 与输出 map key 是不同概念：多个输出可以共享外层类型但必须有不同 map key。不能把两者合为一个字段，也不应强制 key 等于 logical_name。Adapter 声明多个合法 biz 契约同样不属于这项兼容回退。

### A5. 删除无效果的 --no-default-control

**事实。** [demo_options.h](../../demo/common/demo_options.h) 第 42–43 行明确标为 Compatibility；[demo_options.cpp](../../demo/common/demo_options.cpp) 第 237 行解析它，249 行专门检查其与 `--example-control` 冲突，595 行帮助也称兼容参数。默认已经不发送示例 Control，因此在正常 CLI 路径中该参数没有额外作用。[operator_runner.h](../../demo/common/operator_runner.h) 第 182 行仍检查它。

工具仍主动传递这个空操作参数：[verify_selection.py](../../tools/verify_selection.py) 第 236 行、[Studio server](../../tools/pipeline_studio/server.py) 第 562 行。

**建议与验收。** 删除字段、解析、冲突分支、帮助和所有内部传参，更新 Demo/Studio 测试和教程。保留默认行为、显式 `--example-control`、Control 文件优先级及真正的命令路由。通过对照 Demo 默认运行和显式示例 Control，证明清理没有改变有效功能。

### A6. Studio 删除旧 Catalog 字段消费分支

**事实。** [app.js](../../tools/pipeline_studio/web/app.js) 第 245、305 行拒绝非 v3 Catalog，但第 233–234 行仍读取 `model_config_field` / `model_capability`。[workbench.js](../../tools/pipeline_studio/web/workbench.js) 第 10、34、215、230、245 行保留同类回退。

native 当前输出已经只用 `model_dependencies`；[Catalog 契约测试](../../tests/contract/catalog/test_catalog_contract_ssot.cpp) 第 219–220 行明确断言旧字段不存在。因此这里是消费者未收尾，不需要再给 native 增加或删除一套旧字段。

**建议与验收。** 所有模型依赖读取统一为 `model_dependencies`，迁移旧字段测试夹具；保留合法的字段语义提示和无模型节点处理。覆盖模型选择、替换、删除、多个模型依赖，以及拒绝非 v3 Catalog。此项主要降低前端维护成本。

## 5. P2：结构重复与接口范围收缩

### B1. 统一 Core 诊断码，消除往返降维

**事实。** [pipeline_diagnostic.h](../../include/core/pipeline_diagnostic.h) 第 10 行定义 `PipelineErrorCode`；[pipeline_validator.h](../../include/core/pipeline_validator.h) 第 20 行另有 `DiagnosticCode`。实际路径是：

```text
PipelineConfig 解析错误
  → PipelineErrorCode
  → Validator 的 PipelineErrorCodeToDiagnosticCode
  → DiagnosticCode
  → Pipeline::Build 的 ValidationCodeToPipelineCode
  → PipelineErrorCode
```

转换位置为 [pipeline_validator.cpp](../../src/core/pipeline_validator.cpp) 第 107 行和 [pipeline.cpp](../../src/core/pipeline.cpp) 第 21、483 行。多个精确错误被降为 `kInvalidCombination`，Build 又在第 485 行把原码拼入 message。这是可观察的重复表示和信息补偿；当前源码不足以证明它完全由历史兼容造成。

**建议。** 使用一个 Core 诊断码集合，覆盖解析、校验、物化和生命周期。单条 Build 错误与多条校验报告可以保留不同容器，避免让所有路径携带整份报告。C ABI 状态映射继续留在 Integration 边界。

**验收重点。** 同一错误在解析/校验/Build 中具有一致身份；CLI 的稳定错误码、JSON Pointer、关联节点与修复信息不丢失；[shared_algorithm_runtime.cpp](../../src/adapter/shared_algorithm_runtime.cpp) 第 170、259 行对注册冲突的状态映射不退化。涉及 Core C++ 契约，应有 RFC。

### B2. Node 的 creator 和 Definition 放入同一注册 Entry

**事实。** [node_registry.h](../../include/core/node_registry.h) 第 134 行持有 creator 表，而 [pipeline_catalog.cpp](../../src/core/pipeline_catalog.cpp) 第 18、28 行另持 Definition 集合和锁。[node_registry.cpp](../../src/core/node_registry.cpp) 第 32–50 行先锁 Registry，再注册 Catalog Definition，再保存 creator。Validator 第 1188–1189 行分别查两侧。

Model/Backend 已在单个 Entry 同时持有 creator 与 Definition，见 [model_registry.h](../../include/engine/model_registry.h) 第 60 行和 [backend_registry.h](../../include/engine/backend_registry.h) 第 49 行。Catalog 对它们执行查询委托。

**建议。** Node 采用同样的单所有者结构；Catalog 提供排序、快照和序列化视图，停止单独维护可注册的 Node 状态。保留 Definition 的纯校验函数供测试调用。不要再为三个 Registry 引入复杂的万能注册框架，先解决 Node 的具体双表问题。

**收益与风险。** 减少一致性状态和嵌套锁约束，但属于高风险所有权重构，需要 RFC 和独立复核。Control 命令跨 Node 一致性审计、静态注册异常屏障、失败后 fail-closed、冲突检测及 creator 可重入能力仍要保留。不能因存储合并而删除这些约束的测试。

### B3. 清理 Model 查询和 Node 内部闲置接口

**候选一：IModel::GetMaxBatchSize。** [model_interface.h](../../include/engine/model_interface.h) 第 26 行强制所有 Model 实现此查询；`src/` 内只有定义，没有查询消费者。可以通过明确接口决策删除统一纯虚查询，减少每个模型及 mock 的必实现方法，让批处理能力留在实际执行路径。

它不是纯死代码：[dev_support/inference/test_capability_models.h](../../dev_support/inference/test_capability_models.h) 第 39、96 行及部分测试仍用它构造策略；[RFC-0036](../rfcs/0036-whisper-asr-backend.md) 第 176 行也定义过它的语义。仓内无生产调用不证明仓外扩展无人使用。修改涉及 Model 源码扩展契约，优先级低于明确旧语法清理。

迁移测试时改为局部常量或私有 helper，保留 Backend `GetBatchPolicy()`、模型限额约束和 `FixedBatchExecutor::Execute`。[BGE Embedding](../../src/engine/models/bge_embedding/bge_embedding_model.cpp) 第 172、230 行存在真实批策略校验及执行。验收重点是批次上限、padding 去除和来源对应，不能仅删除查询断言。

**候选二：GetRawBatch。** [function_node.h](../../include/nodes/function_node.h) 第 326 行声明此虚接口，第 406 行有唯一实现，全仓相关代码没有调用。可随其他修改删除裸 `const void*` 暴露，保留实际使用的 `HasBatch`、typed binding 和来源读取。收益只有数行及一个虚方法，不应包装成大幅减重。

### B4. Session 收缩便利查询与资源变更入口

**事实。** [session_context.h](../../include/core/session_context.h) 中以下接口当前没有调用：第 195 行 `GetAllRegistrations()`，226–230 行 `GetChipType()`、`GetPlatformMaxBatch()`、`GetDepthNum()`。第 174 行 `UpdateModelRevision()`、184 行 `GetAllModels()`、233/250 行 `SetResource/GetResource` 的调用集中在测试。

**建议。** 先删除确认零调用的便利接口。对仅测试使用的可变入口单独决定：模型 revision 是否允许运行中变更、通用资源是否真的需要覆盖写。若当前产品不支持这些操作，可把必要夹具能力移到测试支持代码，缩小生产 Session 操作面。

**保留与限制。** `GetOrCreateResource()` 和 `GetModelRevision()` 有真实消费者：[text_embedding_node.cpp](../../src/common_nodes/text_embedding_node.cpp) 第 61、66 行用它们做缓存，不能删除资源机制或 single-flight。`RegisterModel()` 虽主要服务测试/benchmark，却只是短小便利包装，保留它可能比迁移大量夹具更划算。这里不建议仅因 getter 无人用，就删除平台结构字段或 RuntimeOptions 中的同名参数。

## 6. P3：需要范围决策，不能直接当作死代码

### C1. 是否取消 kPrivateExtensionCompatible

**事实。** [pipeline_validator.h](../../include/core/pipeline_validator.h) 第 15 行公开严格/私有扩展双策略。[pipeline_validator.cpp](../../src/core/pipeline_validator.cpp) 第 1042、1069、1078、1190、1272 行据此区别处理未知 biz 或缺 Definition。历史 [RFC-0008](../rfcs/0008-architecture-contract-consolidation.md) 第 89、250 行明确为私有 C++ 扩展和内部调用保留该策略。

当前显式选择宽松模式的仓内调用都在测试及 `tests/support/node_harness.h`；正常产品入口使用严格模式。Node 注册已经拒绝空 Definition，Model/Backend 也把 Definition 与 creator 一起存储，许多“只有 creator、没有 Definition”的兼容状态已不符合正常注册入口。

**仍有真实用途。** [test_validated_pipeline_plan.cpp](../../tests/unit/core/test_validated_pipeline_plan.cpp) 第 367 行起的 `StrictVsCompatiblePolicy` 证明宽松模式允许未注册测试 biz；[NodeHarness](../../tests/support/node_harness.h) 第 216、287 行构造 `harness_biz` 并使用宽松策略。Validator 第 1412 行附近也因 biz 不存在而不执行同样的 ingress 闭合检查。因此不是整条路径不可达。

**条件建议。** 先清理当前正规注册不可能产生的缺 Definition 兼容分支。若确定所有可交付 Pipeline 都必须有完整 biz，再把测试迁移到明确的最小测试契约，补齐 ingress/egress 和节点 biz 约束，随后取消宽泛策略。不能简单全局改成 strict。若保留无 Integration biz 的嵌入式产品场景，应明确其范围，不用“兼容”名称无限容纳不完整状态。

### C2. 收敛 Node 初始化与脚手架作者路径

**有计划/无计划初始化。** [model_bound_node.h](../../include/nodes/model_bound_node.h) 第 54–81 行，以及 [function_node.h](../../include/nodes/function_node.h) 第 360、642、713、1129、1321 行附近，为 `init_ctx.plan == nullptr` 保留逻辑端口直连、重新规范化配置或模型字段解析。这是可见的第二条初始化路径。

独立 Node 单测和作者直接初始化依赖它，因此不能仅按重复解析认定为历史垃圾。若统一要求所有 Node 带计划，应由 NodeHarness/作者工具构造最小计划后删除无计划分支；若直接初始化是长期能力，则仅合并两路共用机制。不要迫使单节点测试加载完整 Pipeline、Adapter 和模型系统。

**脚手架模式重叠。** [traceable_unary_inference_node.h](../../include/nodes/traceable_unary_inference_node.h) 共 91 行，当前没有内置 `src/` 派生类，直接实例化在 [test_node_base_contracts.cpp](../../tests/unit/core/test_node_base_contracts.cpp) 第 406、420 行。但它不是只有测试使用：[scaffold_custom_node.py](../../scripts/scaffold_custom_node.py) 第 186、269、319 行仍为 `--kind unary_inference` 生成继承代码和 include；第 1188 行作者模式默认仍为 advanced。basic 模式第 138–155 行也没有覆盖全部模型能力。

建议先比较并收敛脚手架 `model` / `unary_inference` 作者路径，更新生成结果及作者指南，再评估移除中间模板。保留空批次、模型错误、输出数量和 provenance 检查。**不能以 src 无派生类为依据直接删掉这个头，也不能仅删专属测试而让生成器继续输出失效代码。**

### C3. 测试装配只维护一份声明

**事实。** 根 [CMakeLists.txt](../../CMakeLists.txt) 第 260 行提供 `LLM_EDGEFLOW_SHARDED_TEST_RUNNERS`；[Tests.cmake](../../cmake_ext/Tests.cmake) 第 17–22 行切到另一套 370 行的 [IndividualTests.cmake](../../cmake_ext/IndividualTests.cmake)。两者重复维护 ABI/静态门禁、可执行目标、链接、标签和工作目录。[TestInventory.cmake](../../cmake_ext/TestInventory.cmake) 共享源清单，但没有消除注册规则重复。canonical gate 固定使用 sharded。

**反证。** [测试指南](../../tests/README.md) 第 22 行明确 individual 用于逐文件独立进程诊断，它不是单纯旧格式兼容。如果诊断需求存在，不能只为删 370 行去掉能力。

**建议。** 优先让一份测试声明生成两种装配；或者取消全量 individual 开关，仅保留确实依赖独立进程的注册冲突、生命周期等 fixture。验收比较测试清单、标签、环境及隔离要求，不能以删测试获得更快门禁。收益是构建矩阵和维护成本，不直接减少生产 SDK。

### C4. 工具和治理的局部收敛

1. **删除 Studio 指纹别名及猜测式测试。** [server.py](../../tools/pipeline_studio/server.py) 第 325–326 行 `tool_fingerprint()` 只转调 `get_tool_fingerprint()`；[test_pipeline_studio.py](../../tests/tooling/test_pipeline_studio.py) 第 205–218 行却用 `hasattr`、callable/property 探测、模块级兜底、吞异常和常量兜底寻找它。测试直接调用当前方法即可，让接口失配明确失败。
2. **include/vendor 归属规则单点维护。** [check_layer_isolation.sh](../../scripts/check_layer_isolation.sh) 第 204、217 行的跨层 include 正则，与 [check_layer_dependencies.py](../../scripts/check_layer_dependencies.py) 第 56–79 行部分重叠；shell 第 463 行最后又调用 Python。把相同规则集中到 Python，shell 保留协调职责。保留第 465–467 行真实 CMake 头视图编译探针，它和静态扫描覆盖不同失效方式。
3. **压缩历史禁用项检查。** [check_architecture_docs.sh](../../scripts/check_architecture_docs.sh) 第 68–85 行，以及 LayerGuard 第 292–303、454–459 行，为已删除名称维护专门规则。可用小型禁止项表代替散落逻辑和重复输出；必要拒绝测试继续保留。这些代码是在拒绝旧架构，不能作为框架仍支持旧架构的证据。

## 7. 不建议因“未生产”而直接删除的机制

| 机制 | 保留依据 |
| --- | --- |
| C ABI 与 Operator 两个入口 | [c_api_adapter.cpp](../../src/adapter/c_api_adapter.cpp) 第 95 行和 [operator_adapter.cpp](../../src/adapter/operator/operator_adapter.cpp) 第 372 行共用 SharedAlgorithmRuntime::ExecuteBatch；Operator 另承担命名 I/O、宿主载体和输出池。没有发现两份独立推理运行时 |
| Adapter 与 Operator Bridge | 分别承担业务解包/组装和宿主载体/所有权适配。相同 C struct 不等于相同 JSON 契约；Translate 的完整请求/响应转换仍须在 Adapter，不能转移到 Demo |
| C11 公开头、六个 Alg_* 异常屏障、输出池与容量检查 | 它们保证语言边界和内存生命周期，不属于历史兼容语法。破坏旧名称兼容不意味着可以削弱这些机制 |
| 单一 Validator、ValidatedPipelinePlan 和显式 DAG | [pipeline_config.cpp](../../src/core/pipeline_config.cpp) 第 510 行要求显式字段；[pipeline.cpp](../../src/core/pipeline.cpp) 第 475 行调用 ValidateAndPlan。未发现旧顺序 Pipeline 自动转换或运行时再次排序 |
| Studio 轻量编辑检查 | [server.py](../../tools/pipeline_studio/server.py) 第 328–340 行调用 native validate --stdin；前端端口/成环提示用于交互，不能声称它复制了完整 Validator |
| Model/Backend 分离、中性协议、批策略和来源校验 | 多种 Model/Backend 已实际注册；FixedBatchExecutor 承担 padding 去除和 (req_id, sub_id) 保持。多个 Backend 的相似调用壳不等于算法重复 |
| 配置快照和会话 single-flight | 有真实节点消费者和并发/生命周期覆盖。RFC-0052/0054 仍为 In Implementation，主要剩真实开发者体验验收，不能据状态误报工程迁移未完成 |
| mock/test Backend、fixtures 和历史 RFC | mock 支持目标单独链接，不能把其源码行数算进生产 SDK 膨胀。历史 RFC 保存决策和验证基线，适合归档及减少重复现行指南，而非整体删除 |

GeneratedTextEmbedding 等实验能力有配置/协议及测试依据，当前证据不足以认定冗余。如果想通过砍模态、Backend、Studio 或 Demo 大幅缩小产品范围，应先明确首个生产场景和交付集合并测量产物；这属于产品裁剪，不应混入兼容清理当作无损删除。

同样，不建议因 `function_node.h` 约 1,623 行、`pipeline_validator.cpp` 约 1,977 行就删除函数式作者接口或修复建议。可讨论拆分文件和减少模板实例化，但仅拆文件不会减少实现量；Explain 当前用同一个 Validator 检查候选修复，属于复用。

## 8. 建议实施顺序与退出条件

### 第一批：兼容入口收口

范围为 A1–A6，可搭配 B3 的 GetRawBatch 和 C4 的指纹别名清理。先记录破坏性契约，再原子更新仓内实现、配置、生成器、Demo、测试和现行文档。旧头等微收益项可随主要改动一起完成，不必各自开启大型重构。

退出条件：旧格式不再有成功执行路径；当前 Catalog/配置/模板/Bridge 只用规范表示；所有官方方案通过对应构建检查和 smoke；旧格式失败明确可诊断。必要的旧输入拒绝案例留在现有套件，不继续维护旧格式成功测试。

### 第二批：内部表示与扩展面收敛

按 B1 → B3/B4 → B2 分开实施。诊断码统一和闲置接口删除可较局部验证；Node Registry 所有权合并应单独 RFC、单独审查，并做异常/冲突/可重入验证。不要同时改诊断、锁和注册生命周期，避免出问题后难以定位。

退出条件：每项变更都能展示净减少的字段、分支或必实现接口；输出诊断与失败行为不退化；没有用新的兼容适配层抵消删除收益。

### 第三批：依据真实扩展和诊断需求裁剪

对 C1–C3 明确选择：是否支持无 biz 嵌入式 Pipeline、是否支持无计划 Node 初始化、脚手架保留哪些作者模式、是否需要完整 individual 测试模式。默认倾向一个可交付契约，但测试便利不应通过生产宽松开关无限扩散，也不应因收敛而让单测加载无关子系统。

涉及公共契约、兼容策略、跨层所有权和生命周期的实施遵循 [CONTRIBUTING.md](../../CONTRIBUTING.md)，用新 RFC 明确取代旧决策的范围。[RFC-0024](../rfcs/0024-pre-release-contract-cleanup.md) 已做过一轮兼容删除，而 [RFC-0046](../rfcs/0046-naming-and-header-boundaries.md) 又有意保留旧头；不能假设所有已完成 RFC 永远要求保留或永远禁止兼容。

每批完成后运行一次 canonical gate `./scripts/run_all_tests.sh`；非默认 Backend 或并发语义被触及时另做所需专项验证，不重复运行无关完整门禁。

## 9. 如何衡量是否真的瘦身

逐批记录公开入口数、配置可接受形式数、兼容条件分支数、Node/Model 必实现接口数、重复状态存储数、测试装配声明数和净代码行数。性能结论另用相同构建选项测量库大小、构建耗时、初始化耗时及代表性请求耗时。

不要预先承诺删除若干千行或体积降低若干百分比。本轮最明确的可量化对象包括七个旧头、一个旧别名、三模式模板选择、25 份旧 .conf 的迁移、两种输出回调位置、Studio 旧模型字段分支。它们收益不同，不能简单相加成运行时收益；91 行的 TraceableUnaryInferenceNode 则必须先迁移脚手架才可能删除。

## 10. 本次审查验证记录

已完成基线与工作区状态确认、Catalog 查询、符号及配置使用检索、整文件内容比对、生成器/相关测试/RFC 静态核对，并对易误删候选进行了独立交叉复核。报告保存于独立本地文档分支，仅新增本报告并更新归档索引。

验证结果：

- `LLM_EDGEFLOW_JOBS=4 ./scripts/run_all_tests.sh`：退出码 0，97/97 个 CTest 测试通过，失败 0；门禁耗时 38 秒，CTest 耗时 26.58 秒。包含默认配置构建、格式/静态检查和全部注册测试，没有另跑重复完整门禁。
- `git diff --check`：通过。
- 报告独立检查：71 个相对文件链接全部存在，无行尾空白，文件末尾有换行；补录本验证记录后再次检查链接及空白。
- 完整命令日志保存在本机 `/tmp/framework-slimming-gate-2026-09-14.log`，未作为项目源码提交。

以上结果验证的是当前基线及本次文档交付，不证明尚未实施的删除建议已经安全，也不构成真实模型效果、非默认 Backend 或目标设备验收。

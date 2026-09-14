# 第一批框架瘦身实施复核

- 复核日期：2026-09-14。
- 对照材料：[投产前框架冗余、兼容负担与瘦身审查](FRAMEWORK_SLIMMING_REVIEW_2026-09-14.md)。
- 当前 HEAD：`09437485aec0704175cdc3acbdf73c8a9b47891e`；实现位于暂存区，未提交。
- 待审范围：相对 HEAD 的 88 个文件，701 行增加、734 行删除。本报告及归档索引不计入该实施范围。
- 暂存补丁标识：`git diff --cached --binary` 的 SHA-256 为 `0dc7cbc4e40884388ab1f6e42448bd323f77d1937992e23944f5f7654cbfa8f6`。
- 本轮仅复核并保存结论，不修改用户的实现、测试或暂存内容。以下源码行号对应本次待审工作区。

## 1. 结论

**第一批实施方向基本符合预期，主体迁移完成，但尚不能判为全部收尾。确认一处自定义输出槽的行为回归，以及 Catalog 描述、活动指南、回归测试和迁移决策记录的遗漏。**

统一门禁已经通过：97/97 个 CTest 测试成功。这个结果说明当前门禁没有发现失败，不排除测试未覆盖的扩展场景；本轮输出 key 回归已通过新旧头文件独立探针确认。

Changelog 明确将此次实施称为“第一批瘦身落地”。因此，原报告 B1/B2 和 C1–C3 尚未实施属于后续范围，不应把它们列作本批回归，也不能据此宣称原报告全部建议已完成。

## 2. 需要修正的发现

### F1 [P2] Typed Bridge 自定义逻辑输出槽意外改变外部 map key

**位置：** [operator_biz_bridge.h](../../include/adapter/operator_biz_bridge.h) 第 125–126、166–167、224–225 行。

`MakeSingleSlotBizBridge` 现在把 `key_suffix` 显式设为传入的 `output_slot`。两种 `MakeTypedSingleSlotBizBridge` 随后只将 `type_suffix` 更新为 `HostOutput` 的规范类型，没有同步确定原有外部 key。

传入逻辑槽名 `custom_out`、输出宿主类型 `CompanyOperatorEntityOutput` 时，独立探针对比结果如下：

| 实现 | logical_name | type_suffix | KeySuffix() |
| --- | --- | --- | --- |
| HEAD 旧实现，两种 typed helper | custom_out | entity_out | entity_out |
| 当前实现，两种 typed helper | custom_out | entity_out | custom_out |
| 默认槽名，新旧实现 | entity_out | entity_out | entity_out |

原有外部 `chan.entity_out` 因而不能匹配当前描述符。[operator_process_binding.cpp](../../src/adapter/operator/operator_process_binding.cpp) 第 135 行按 `KeySuffix()` 比较，第 157–163 行在找不到必需输出 key 时返回 `-4`。

原报告 A4 要求 helper 显式填写“同样的输出 key”，并明确逻辑槽名、类型和外部 key 是不同概念。此次变化超出了移除隐式回退的范围。内置桥大多使用默认槽名，所以当前 smoke 和一般业务测试未触发。

**建议：** 两种 typed helper 在确定 `HostOutput` 类型后，同时显式设置原先使用的规范输出 key；若调用方需要不同外部 key，继续通过槽位字段明确指定，不恢复运行时回退。扩展现有 [TypedBuilderSupportsCustomInputSlotName](../../tests/unit/operator/test_operator_biz_bridge_registry.cpp) 第 877 行起的测试：它已经传入 `custom_out`，但只断言输入转换，应补两种 helper 的输出 key、输出绑定及显式自定义外部 key 覆盖。

**复现记录：** 在 `/tmp/slimming-bridge-probe-f_benlzm/` 保存独立 `probe.cpp` 和通过 `git show HEAD:include/adapter/operator_biz_bridge.h` 提取的旧头。分别用 C++17 编译并运行旧头/当前头探针，两者编译成功，输出如上；未修改项目测试或构建目录。

### F2 [P2] TextTemplate 的运行时 Catalog 仍宣称支持单括号变量

**位置：** [text_template_node.cpp](../../src/common_nodes/text_template_node.cpp) 第 627–629 行。

`NodeDefinition.description` 仍输出：

```text
Text template rendering: {{name}} and {name} substitute variables;
JSON braces remain literal
```

而共享解析器已正确收敛为仅 `{{name}}` 是变量。重建后查询 `alg_pipeline_tool catalog`，上述旧描述仍然存在；独立解析器探针实际返回：

```text
Hello {name}   => ok=1, variables=0
Hello {{name}} => ok=1, variables=1
```

这不是仅存在于历史文档中的文字。Catalog 是当前开发者和编排工具使用的能力事实来源，按其说明写单括号模板，会得到成功执行但没有变量替换的结果。

**建议：** 更新 Definition 描述，明确单括号只是字面文本，并同步 [platform_mock/operator_types.h](../../include/platform_mock/operator_types.h) 第 57 行仍写单括号占位符的注释。现有新语法行为测试保留；重建后核对 `describe-node TextTemplateNode` 或 Catalog 输出。

### F3 [P3] 活动指南仍承诺已经删除的用法

| 位置 | 遗留说明 | 当前行为及修正方向 |
| --- | --- | --- |
| [Operator 输出分配指南](../dev_guide/operator_output_allocation.md) 第 39 行 | key_suffix “省略时沿用 type_suffix” | Registry 已要求非空。应说明手写描述符必须显式填写、与逻辑槽及类型独立，以及 helper 的默认规则 |
| [pipeline-composer SKILL.md](../../.agents/skills/pipeline-composer/SKILL.md) 第 82–83 行 | --no-default-control 仍是可用兼容参数，且与 --example-control 冲突 | CLI 现在把它作为未知参数拒绝。应删除可用性说明，保留默认不发送示例 Control 和显式 --example-control 的用法 |

这些都是现行操作指导，按前者编写扩展可能导致注册失败，按后者保留旧参数会导致 Demo 参数解析失败。历史 RFC 和原审查报告里的旧语法无需改写。

### F4 [P3] 新增 AdapterName 负向测试没有证明旧 biz_name 回退已被删除

**位置：** [test_operator_biz_bridge_registry.cpp](../../tests/unit/operator/test_operator_biz_bridge_registry.cpp) 第 674–689 行。

新增 `IsolatedRegistryRejectsAdapterNameMismatch` 使用 `doc_qa_v1` 作为错误标识。但 [doc_qa_adapter.cpp](../../src/adapter/biz/doc_qa_adapter.cpp) 第 15 行声明的实际 biz 是 `smart_doc_qa_v1`，AdapterName 是第 23 行的 `DocQA`。

`doc_qa_v1` 在旧实现中同样不能匹配 AdapterName 或声明的 biz，所以该输入不能区分旧回退实现和当前严格实现。当前生产代码确实移除了回退；问题在于这项新增测试无法防止它再次被引入。

**建议：** 将待拒绝值设为 Adapter 实际声明的 `smart_doc_qa_v1`，最好从描述符获取，使测试证明“合法 Pipeline biz_name 也不能作为 AdapterName”。保留规范 AdapterName 注册成功的既有覆盖。

### F5 迁移决策记录未完成

本次实现没有新增或更新 `doc/rfcs/` 下的迁移 RFC。它已经涉及公开源码头入口、模板配置语义、Operator 输出配置和 Bridge 扩展契约，符合 [CONTRIBUTING.md](../../CONTRIBUTING.md) 第 17、32–39 行的 RFC 范围。

尤其 [RFC-0046](../rfcs/0046-naming-and-header-boundaries.md) 曾明确保留旧公共头，[RFC-0049](../rfcs/0049-operator-output-allocation-strategies.md) 曾明确保留旧输出配置及回调/key 默认行为。Changelog 记录了发生什么，但尚未明确哪些旧兼容决策被取代。

**建议：** 新建本次迁移 RFC，明确第一批范围、规范契约、被取代决策、配置/扩展迁移规则及验证结果；保留历史 RFC 正文。将剩余 B/C 项标为后续或有意保留，不把此次审查当作已批准的全部架构变更。

## 3. 对原报告逐项核对

| 原编号 | 当前状态 | 依据与剩余范围 |
| --- | --- | --- |
| A1 旧公共头与 NodeFactory | 主体完成 | 七个旧头、别名及构建清单已删；规范 edgeflow/operator/types.h 和 C11/头可见性保护保留 |
| A2 模板语法 | 实现完成，元数据未收尾 | 私有旧解析器和 template_syntax 已删，默认值、fixture、缺变量及 JSON 字面行为已迁移；剩 F2 |
| A3 data.outputs | 完成 | 配置、生成器、Studio 保存/运行及效果工具已迁移；旧字段拒绝和多输出校验保留 |
| A4 Bridge 单路径 | 主体完成，有回归 | 槽位回调、严格 AdapterName、显式 key 均落地；剩 F1、F3、F4 |
| A5 Demo 参数 | 实现完成，skill 未收尾 | 字段、解析、帮助、冲突判断和内部传参已删；剩 F3 |
| A6 Catalog 旧字段 | 完成主体清理 | 前端移除 model_capability/model_config_field，使用 model_dependencies；单依赖选择/改名/删除保护及版本拒绝保留 |
| B1 Core 诊断统一 | 未实施 | 两套枚举及双向映射仍在，应作为第二批独立改动 |
| B2 Node 注册状态统一 | 未实施 | Registry/Catalog 分离存储仍在，保留高风险独立设计和验证要求 |
| B3 Model/Node 闲置接口 | 部分完成 | GetRawBatch 已删；GetMaxBatchSize 仍在，等待明确接口决策 |
| B4 Session 范围 | 部分完成且符合第一步 | 四个零调用便利查询已删；revision 修改、资源读写等有测试/扩展含义的接口仍保留 |
| C1 宽松 Validator 策略 | 保留 | 测试 Harness 仍依赖，不能简单替换为 strict |
| C2 Node 初始化/作者路径 | 保留 | 无计划初始化与 TraceableUnaryInferenceNode/脚手架未误删 |
| C3 双测试装配 | 保留 | 应另行决定是否统一声明或收缩诊断模式 |
| C4 工具和治理 | 部分完成 | 指纹别名及猜测式测试已清理；静态 include 规则集中、历史禁用项表尚未实施 |

A6 多模型依赖专项测试尚未找到，当前循环实现未见明显错误，可作为验收补强，不列为已证实运行时缺陷。

保留 C ABI/Operator 共享运行时、完整业务 Adapter 转换、输出池生命周期、批策略、来源校验和配置快照符合原报告预期。没有发现本次为瘦身而删除这些必要机制。

## 4. 实际验证

1. `LLM_EDGEFLOW_JOBS=4 ./scripts/run_all_tests.sh > /tmp/framework-slimming-implementation-gate.log 2>&1`：退出码 0，97/97 个 CTest 测试通过，门禁耗时 47 秒。仅执行一次完整门禁。
2. `git diff HEAD --check`：通过，覆盖暂存实施变更。
3. 重建后 `./build/alg_pipeline_tool catalog`：退出码 0、schema_version=3、ok=true；12 Nodes、6 Models、8 bizs、14 Profiles，2 Backends（llama_cpp、onnxruntime）。快照位于 `/tmp/framework-slimming-rebuilt-catalog.json`。
4. 所有 27 份变更 `.conf` 做 JSON 等值对比：只将旧池配置放入 `outputs` 对应槽，模型路径、容量和其余字段全部一致；其中包括原报告关注的 25 份 configs/Demo 文件和额外 2 份测试配置。
5. 新旧 typed Bridge 头文件独立探针：确认 F1；默认槽名不受影响，自定义逻辑槽名会改变 key。
6. 当前模板解析器独立探针 `/tmp/slimming-template-probe.cpp` 与重建 Catalog 对照：确认 F2。
7. 报告及归档索引另做相对文件链接和空白检查；没有修改、重新暂存用户实施内容。

门禁默认关闭 Kite/Whisper Backend 和专门的真实模型 E2E 验证，不能将本次结果解释为全部 Backend、真实模型效果或目标设备验收。此次配置等值对比也不替代那些环境中的部署运行。

## 5. 收尾建议

优先修 F1 并在现有 Operator 测试中补输出 key/绑定断言；同步处理 F2/F3 的当前说明和 F4 的鉴别性回归用例，补 F5 的迁移决策。修改后按项目流程重新执行所需验证和一次统一门禁。

第一批通过这些收尾后可以独立验收。B1/B2、剩余 B3/B4、C1–C3 和 C4 静态治理合并应继续按原报告分批决定，不需要为了宣称“全部瘦身完成”而在同一改动中扩大范围。

## 6. 修复后复验（2026-09-14）

本节对应用户根据上述发现完成修复后的工作区；前文保留原审查时的事实与结论。复验暂存补丁 SHA-256 为 `42b678c300585aa13abd76df42e791860053eda6398c4dec444e48e9b267a9b1`。

**复验结论：F1–F5 均已解决，本次未发现新的阻断问题；第一批瘦身符合预期，可以按本批范围验收。**

| 原发现 | 复验结果 |
| --- | --- |
| F1 typed Bridge 输出 key 回归 | 两种 typed helper 都在构造时明确填入规范输出 key；自定义逻辑槽不改变默认外部 key，显式自定义 key 也保留。测试覆盖成功绑定、错误 key 拒绝及第二重载；运行时 KeySuffix 没有恢复回退，Registry 仍拒绝不完整描述符 |
| F2 TextTemplate Catalog 描述 | Definition 和公开模拟头注释已更新；新增描述检查，重建后的 Catalog 明确仅双括号替换，单括号和 JSON 花括号保留字面内容 |
| F3 活动指南残留 | 输出分配指南明确非空 key_suffix；pipeline-composer 已删除旧 CLI 参数可用性说明 |
| F4 AdapterName 回归测试缺乏鉴别性 | 测试从实际 Adapter Definition 取得 smart_doc_qa_v1，并增加全业务检查；能发现旧 biz_name 回退被重新引入 |
| F5 迁移决策缺失 | [RFC-0056](../rfcs/0056-batch-1-framework-slimming.md) 和索引已补齐，明确取代旧兼容决策及第一批边界 |

验证证据：

- 独立复核 Bridge 的两个 helper、运行时拒绝规则及新增输出绑定/AdapterName 测试，未发现新的回归。
- `LLM_EDGEFLOW_JOBS=4 ./scripts/run_all_tests.sh > /tmp/framework-slimming-fixes-gate.log 2>&1`：退出码 0，97/97 个 CTest 测试通过；门禁耗时 35 秒，CTest 耗时 31.19 秒。仅执行一次完整门禁。
- `git diff HEAD --check` 通过；验证前后暂存补丁 SHA-256 一致。
- 重建 Catalog（schema_version=3）保存于本机 `/tmp/framework-slimming-fixes-catalog.json`，TextTemplate 描述为 `Text template rendering: {{name}} substitutes variables; single {name} and JSON braces remain literal`。
- 新 RFC、输出分配指南与 pipeline-composer 的相对文件链接检查通过。本复验记录补录后另做链接与空白检查，未改动或重新暂存用户实现。

原报告第二、三批的设计收敛与产品范围选择仍属于后续工作，本次未扩大验收范围；也未新增真实模型、非默认 Backend 或目标硬件验收结论。

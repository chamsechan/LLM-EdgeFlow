# RFC 0056: 投产前框架兼容入口与配置收口（第一批瘦身落地）

- **RFC 编号**：0056-batch-1-framework-slimming
- **创建日期**：2026-09-14
- **文档状态**：Completed
- **关联分支**：`docs/framework-slimming-review-2026-09-14`
- **目标版本**：投产前基线
- **负责人 / 作者**：LLM-EdgeFlow Team
- **关联决策**：
  - 取代 [RFC-0046](0046-naming-and-header-boundaries.md) 中保留旧公共头转发（`company_alg_*.h`、`operator/*.h`）的决策；
  - 取代 [RFC-0049](0049-operator-output-allocation-strategies.md) 中关于根级 `data.mem_que` 兼容及输出 key/回调隐式回退的决策；
  - 补充 [RFC-0042](0042-studio-and-contract-boundaries.md)（模板语法统一为 `{{name}}`，单括号 `{...}` 保留为字面量）；
  - 补充 [RFC-0045](0045-solution-developer-workflow.md)（清理无效果 CLI 兼容选项 `--no-default-control`）；
  - 补充 [RFC-0053](0053-function-oriented-adapter-authoring.md)（Bridge 描述符回调收敛为槽位级 `convert_output`，`AdapterName` 严格 1:1 匹配，单槽 Helper 填充规范 `key_suffix`）；
  - 保留 [RFC-0029](0029-external-readiness-and-intranet-sdk-migration.md) 的内网迁移与隔离边界。

## 1. 问题与范围

在投产前代码审查（参见 [FRAMEWORK_SLIMMING_REVIEW_2026-09-14.md](../archive/FRAMEWORK_SLIMMING_REVIEW_2026-09-14.md)）中发现，框架经过多轮演进后积累了一批过渡期兼容入口与冗余分支：
1. **旧公共头与兼容别名**：RFC-0046 保留了 7 个历史顶层转发头以及 `NodeRegistry` 内部的 `NodeFactory` 别名，调用方入口未彻底收口。
2. **模板解析与语法分流**：`PromptGuidedLlmNode` 维护私有正则解析器及 `template_syntax` 配置（`auto`/`standard`/`legacy`），占位符支持单双括号混用，与标准 `TextTemplateNode` 存在语义分流。
3. **Operator 输出配置双路径**：RFC-0049 引入槽位级 `data.outputs` 后，根级 `data.mem_que` 历史配置仍被读取和转换，加重维护负担。
4. **Bridge 描述符多入口与隐式回退**：顶层 `convert_sample_output` 与槽位级 `convert_output` 并存；`OperatorBizBridgeRegistry` 允许 `adapter_name` 回退匹配 Pipeline `biz_name`；单槽 Helper 的 `key_suffix` 依赖运行时空值回退。
5. **无效果 CLI 参数与废弃 Catalog 字段**：Demo 保留无效果的 `--no-default-control` 参数；Pipeline Studio 仍处理废弃的 `model_config_field` / `model_capability`。
6. **未调用的闲置接口**：`function_node.h` 中的纯虚 `GetRawBatch`，以及 `SessionContext` 中未被消费的 4 个零调用便利查询。

**范围边界**：
本 RFC 作为“第一批瘦身落地”，仅覆盖上述 A1–A6、B3（部分）、B4（部分）、C4.1 明确确认的冗余项。
对于审查报告中列出的 B1（Core 诊断统一）、B2（Node 注册状态统一）、剩余 B3/B4、C1–C3（宽松 Validator、双测试装配等）以及 C4 静态 include 集中化规则，保持现状或留待后续独立评估，不纳入本批次，避免在同一改动中扩大架构风险。

## 2. 决策与权衡

### 2.1 接入适配层（Integration）
- **彻底删除 7 个历史转发头**：删除 `company_alg_interface.h`、`company_alg_cpp.hpp`、`company_alg_export.h`、`company_alg_log.h`、`company_alg_version.h`、`operator/operator_interface.h`、`operator/company_operator_types.h`。SDK 公共入口唯一收敛至 `include/edgeflow/` 下的规范 C11 头与 Operator 头。
- **Operator 输出配置单路径收口**：下线根级 `data.mem_que`，统一使用槽位级 `data.outputs.<slot_name>` 配置；彻底移除遗留字段过渡期脚手架，含 `data.mem_que` 的配置直接按未知字段统一 Fail-Closed 拒绝。
- **Bridge 描述符回调与标识收敛**：
  - 输出转换回调统一收敛至每个输出槽位独立的 `convert_output`，移除结构体顶层的 `convert_sample_output` 回调及其执行期 fallback。
  - `OperatorBizBridgeRegistry::GlobalInit` 严格要求描述符的 `adapter_name` 与 `IBizAdapter::AdapterName()` 完全相等，彻底移除遍历 `biz_definitions` 回退匹配 `biz_name` 的分支。
  - 单槽 Helper `MakeSingleSlotBizBridge` 与 `MakeTypedSingleSlotBizBridge` 在初始化时显式填充 `key_suffix` 为规范类型后缀（例如 `entity_out`），消除 `KeySuffix()` 的空值运行时回退；同时保持逻辑槽名 `logical_name`（如 `custom_out`）与外部 key 后缀解耦，并支持通过可选参数 `output_key_suffix` 或字段显式自定义覆盖。

### 2.2 流程编排层（Orchestration）
- 删除 `NodeRegistry` 中的 `using NodeFactory = NodeRegistry;` 历史别名。
- 清理 `SessionContext` 中未被调用的便利查询接口：`GetAllRegistrations`、`GetChipType`、`GetPlatformMaxBatch`、`GetDepthNum`，保留与上下文生命周期、资源与配置紧密相关的核心接口。

### 2.3 能力节点层（Capability Nodes）
- **模板语法统一**：删除 `PromptGuidedLlmNode` 中的私有模板解析器和 `template_syntax` 配置选项，统一调用 `include/nodes/text_template.h` 中的共享 `ParseTextTemplate`。
- **语法规范**：唯一规范为 `{{name}}` 占位符；单括号 `{...}` 保持为字面量（兼容标准 JSON），消除歧义分支。同步修正 `TextTemplateNode` 的 Definition 描述与头文件注释。
- **闲置接口清理**：移除 `include/nodes/function_node.h` 中未使用的纯虚接口 `GetRawBatch`。

### 2.4 模型执行层与工具（Tooling / Demo）
- **Demo CLI 参数收敛**：移除 `--no-default-control` 参数，未知参数保持 Fail-Closed；Demo 默认不发送示例 Control，仅在显式指定 `--example-control` 时触发。
- **Pipeline Studio 收口**：移除针对 `model_config_field` / `model_capability` 的历史兼容逻辑，统一通过 `model_dependencies` 表达模型绑定。
- **清理测试与指纹别名**：删除已废弃的指纹测试别名与基于猜测的冗余用例。

## 3. 兼容与迁移

1. **头文件迁移**：
   - 依赖旧公共头的代码必须迁移至 `#include "edgeflow/c_api.h"` 或 `#include "edgeflow/operator/interface.h"`。
2. **配置文件迁移**：
   - 部署 `.conf` 中所有的 `data.mem_que` 必须迁移为 `data.outputs.<slot_name>`。仓内所有 25 份正式部署配置文件及 fixtures 均已一次性完成迁移。
3. **模板配置迁移**：
   - Prompt 模板中的变量必须使用 `{{name}}` 格式，单括号 `{name}` 将作为字面量文本处理，不被替换。
4. **Bridge 扩展迁移**：
   - 手写注册 `OperatorBizBridgeDescriptor` 时，必须为每个输出槽位设置 `convert_output` 与非空的 `key_suffix`，`adapter_name` 必须精确匹配对应 `IBizAdapter::AdapterName()`。使用 `MakeTypedSingleSlotBizBridge` 时自动填充规范 `type_suffix` 与 `key_suffix`。

## 4. 验证与完成条件

1. **单元与集成测试**：
   - `test_c11_abi_compliance` 验证 C11 ABI 仅暴露规范公共头；
   - `test_operator_biz_bridge_registry` 验证单槽 typed helper 逻辑槽名、规范类型与外部 key 的正确绑定（含可选参数直接指定与缺省规范回填）、全业务负向严格 AdapterName 检查（拒绝各业务真实 declared biz_name）以及非空 key_suffix 约束；
   - `test_text_template_node` 验证单双括号模板语义及 Definition 描述准确性；
   - `test_demo_runner` 验证 CLI 参数拒绝 `--no-default-control`；
   - `test_pipeline_studio.py` 验证配置导出与 Studio 前后端仅依赖 `model_dependencies`。
2. **全局门禁**：
   - 执行 `./scripts/run_all_tests.sh` 全量通过（包含 shell 检查、代码格式、LayerGuard 架构边界检查、全部 CTest 测试通过）。
3. **二进制与 Catalog 检查**：
   - `./build/alg_pipeline_tool catalog` 成功输出有效 JSON，验证节点与业务定义一致性。

## 5. 实施与最终结果

- 实施代码变更覆盖 88 个源码及配置文件，删除 7 个废弃头文件，全仓迁移至规范单路径。
- 复核发现的 5 项收尾问题（typed helper 外部 key 回归、TextTemplate catalog 描述错误、输出分配指南与 skill 遗留说明、负向 AdapterName 鉴别性测试用例、RFC 决策记录缺失）已全部修复并补全验证。
- 自动化门禁 `./scripts/run_all_tests.sh` 运行全部通过。

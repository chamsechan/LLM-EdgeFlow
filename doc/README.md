# LLM-EdgeFlow 文档目录

首次使用从仓库 [README](../README.md#快速开始) 开始。日常开发按下面的任务选择入口；
可用节点、端口、模型、Backend 和业务契约以目标构建的 `alg_pipeline_tool catalog` 为准。

## 按任务开始

先判断外部契约是否变化、现有节点是否足够，再选择需要的路径；同一方案可以组合使用。

| 路径 | 何时使用 | 阅读入口 |
| --- | --- | --- |
| 已有能力编排 | 外部契约不变，调整配置与连线 | [Studio 编排练习](../tools/pipeline_studio/README.md#第一次编排) → [运行当前方案](../tools/pipeline_studio/README.md#运行当前方案)；提示词任务可用[现有 Recipe](dev_guide/recipe_prompt_config.md) |
| 新增自定义 Node | 需要实现缺失算法 | [自定义 Node 入门](dev_guide/first_custom_node.md)，按任务选择 Recipe 或普通脚手架 |
| 新增外部业务契约 | 请求或响应的字段、格式、语义变化 | [业务接入指南](dev_guide/business_onboarding.md)，先复用已有转换器、载体和 Demo 运行代码 |

## 使用开发 Skills

项目技能位于 `.agents/skills/`，可以用 `$技能名` 调用，或让 Agent 按任务选择。
业务描述还没有落到组件时，先用 `$edgeflow-solution-planner`，提供完整请求/响应样例和
业务规则；它会检查当前 Catalog，给出复用/新增部件、实现位置、带类型端口的 DAG 和验收步骤。
已明确修改部件时直接使用对应技能，不必先跑一遍规划。

| 开发任务 | Skill |
| --- | --- |
| 业务拆解、组件选型与 DAG 建议 | [edgeflow-solution-planner](../.agents/skills/edgeflow-solution-planner/SKILL.md) |
| 已有能力的配置与运行 | [pipeline-composer](../.agents/skills/pipeline-composer/SKILL.md) |
| 完整 JSON 输入、提示词处理、完整 JSON 输出 | [json-prompt-solution](../.agents/skills/json-prompt-solution/SKILL.md) |
| Adapter、转换器、业务绑定与输出容量 | [edgeflow-adapter-developer](../.agents/skills/edgeflow-adapter-developer/SKILL.md) |
| Node：逐项变换、文本 LLM、多输入输出、拆分聚合、各类模型能力 | [edgeflow-node-developer](../.agents/skills/edgeflow-node-developer/SKILL.md) |
| Model：预处理与模型语义 | [edgeflow-model-developer](../.agents/skills/edgeflow-model-developer/SKILL.md) |
| Backend：运行时与厂商资源 | [edgeflow-backend-developer](../.agents/skills/edgeflow-backend-developer/SKILL.md) |
| Core、Demo 与跨组件开发 | [llm-edgeflow-developer-guide](../.agents/skills/llm-edgeflow-developer-guide/SKILL.md) |

所有 Node 使用同一种写法（`MakeNodeSpec`）。`common/custom` 按中性操作或领域算法
选择代码归属；Embedding、ASR、OCR、Rerank 复用同一套能力声明，不各自维护生命周期。
例如：“用 `$edgeflow-solution-planner` 分析此请求和响应，列出需要新增的部件与 DAG”；
或“用 `$edgeflow-adapter-developer` 为现有算法补齐新的 JSON 输入输出契约”。

## 进阶与参考

| 资料 | 职责 |
| --- | --- |
| [Node 作者的五个概念](dev_guide/custom_node_concepts.md) | 按需理解端口、来源、模型绑定、Definition 和并发 |
| [第一个 Control](dev_guide/first_control.md) | 给节点增加运行时控制 |
| [Adapter 参考实现](dev_guide/adapter_templates/README.md) | 按输入输出形态查阅已有转换实现 |
| [Operator 宿主类型、输出池与生命周期](dev_guide/operator_output_allocation.md) | 新宿主类型、特殊输出布局与宿主调用规则 |
| [模型、构建与效果验收](VERIFIABLE_SELECTION.md) · [模型资产说明](../models/README.md) | 准备模型、选择构建并验证效果 |
| [架构设计](architecture.md) | 四层职责、编译依赖与运行时数据流 |
| [开发者扩展指南](developer_guide.md) | 按职责查阅进阶接口与扩展约束 |
| [自定义 Node 源码指南](../src/custom_nodes/README.md) | 源码布局、构建登记、测试与跨方案复用 |
| [公共日志 API](logging.md) | C/C++ 日志接入、等级与 Demo 环境变量 |
| [源码布局与命名](dev_guide/source_layout.md) | 仓库顶层目录、四层源码目录、公开/扩展/内部头文件边界与命名约定 |
| [kiteLLM 接入](kitellm.md) | 可选 Backend 的构建、部署示例与验证限制 |
| [开发与交付流程](../CONTRIBUTING.md) · [测试指南](../tests/README.md) | 任务分级、设计审查、验证与交付 |

架构图按用途区分：

- 当前实现类图：[PlantUML 源文件](architecture_classes.puml) · [SVG](assets/architecture_class_diagram.svg)。
- 当前配置与运行流程图：[PlantUML 源文件](architecture_flow.puml) · [SVG](assets/architecture_flow.svg)；区分创建期的准备与校验、请求期的解码与执行。
- 首页工作原理图：[framework_overview.svg](assets/framework_overview.svg)，直接维护 SVG 源码。

前两张 SVG 由 PlantUML `1.2024.7` 生成。修改对应源文件后运行：

```bash
./scripts/render_architecture_diagrams.sh --generate
./scripts/render_architecture_diagrams.sh --check
```

`dev_guide/` 维护操作步骤，架构和参考文档维护当前规则；
[模型、构建与效果验收](VERIFIABLE_SELECTION.md#验收范围与发布准备)说明已验证范围和实际部署需要的证据。
文档维护遵循 [CONTRIBUTING](../CONTRIBUTING.md#5-update-durable-documentation-proportionally)，
同一教程或规则在一个入口维护，其他位置链接引用。

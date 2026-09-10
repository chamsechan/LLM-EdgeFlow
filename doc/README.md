# LLM-EdgeFlow 文档目录

首次使用从仓库 [README](../README.md#快速开始) 开始。日常开发按下面的任务选择入口；
可用节点、端口、模型、Backend 和业务契约以目标构建的 `alg_pipeline_tool catalog` 为准。

## 按任务开始

| 任务 | 阅读入口 |
| --- | --- |
| 用已有节点编排方案 | [Studio 编排练习](../tools/pipeline_studio/README.md#第一次编排) → [运行当前方案](../tools/pipeline_studio/README.md#运行当前方案) |
| 编写第一个自定义 Node | [动手练习](dev_guide/first_custom_node.md) → [Node 作者的五个概念](dev_guide/custom_node_concepts.md) |
| 给节点增加运行时控制 | [第一个 Control](dev_guide/first_control.md) |
| 对接平台输入输出 | [业务接入指南](dev_guide/business_onboarding.md) → [Adapter 参考实现](dev_guide/adapter_templates/README.md) |
| 准备模型、选择构建并验证效果 | [模型、构建与效果验收](VERIFIABLE_SELECTION.md) · [模型资产说明](../models/README.md) |
| 扩展框架、模型或 Backend | [开发者扩展指南](developer_guide.md) · [架构职责与边界](architecture.md) |

## 架构与参考

| 资料 | 职责 |
| --- | --- |
| [架构设计](architecture.md) | 四层职责、编译依赖与运行时数据流 |
| [开发者扩展指南](developer_guide.md) | 按职责查阅进阶接口与扩展约束 |
| [自定义 Node 源码指南](../src/custom_nodes/README.md) | 源码布局、构建登记、测试与跨方案复用 |
| [公共日志 API](logging.md) | C/C++ 日志接入、等级与 Demo 环境变量 |
| [源码布局与命名](dev_guide/source_layout.md) | 公开、扩展和内部头文件边界，以及目录与名称迁移 |
| [kiteLLM 接入](kitellm.md) | 可选 Backend 的构建、部署示例与验证限制 |
| [开发与交付流程](../CONTRIBUTING.md) · [测试指南](../tests/README.md) | 任务分级、RFC 阈值、验证与交付 |

架构图按用途区分：

- 当前实现类图：[PlantUML 源文件](architecture.puml) · [SVG](assets/architecture_class_diagram.svg)。
- 目标演进图：[PlantUML 源文件](architecture_v2.puml) · [SVG](assets/architecture_flow.svg)；按图例区分 Implemented、Partial 与 Planned。
- 首页工作原理图：[framework_overview.svg](assets/framework_overview.svg)，直接维护 SVG 源码。

前两张 SVG 由 PlantUML `1.2024.7` 生成。修改对应源文件后运行：

```bash
./scripts/render_architecture_diagrams.sh --generate
./scripts/render_architecture_diagrams.sh --check
```

## 规划、决策与历史

| 需要了解什么 | 入口 |
| --- | --- |
| 尚待完成的开发者试用与生产验收 | [方案开发者验收计划](plans/solution_developer_acceptance.md) |
| 架构与接口为何这样设计 | [RFC 索引](rfcs/README.md)，优先列出进行中的 RFC |
| 用户可感知的版本变化 | [Changelog](CHANGELOG.md) |
| 特定 RFC 当时的验证证据 | [评审与验收归档](rfcs/reviews/README.md) |
| 日期型审计、整改与已完成计划 | [历史报告归档](archive/README.md) |

`dev_guide/` 维护操作步骤，架构和参考文档维护当前规则，`plans/` 维护尚未完成的工作。
历史 RFC 与报告按原始基线阅读；其中的命令、代码路径、测试数量和阶段待办不代表当前状态。
文档维护遵循 [CONTRIBUTING](../CONTRIBUTING.md#5-update-durable-documentation-proportionally)，
同一教程或规则在一个入口维护，其他位置链接引用。

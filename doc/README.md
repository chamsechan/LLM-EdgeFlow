# LLM-EdgeFlow 文档目录

本目录维护 LLM-EdgeFlow 的架构、开发、接口和 RFC 文档。项目概览与最短上手路径位于仓库根目录的 [README](../README.md)，版本演进摘要位于 [Changelog](CHANGELOG.md)。

- **[architecture.md](architecture.md)**：框架职责划分与编译边界、模块详细职责、运行时时序图（Mermaid）及算子开发上手规范。
- **[architecture.puml](architecture.puml)**：框架代码库当前物理实现的精确 PlantUML 类图（As-Is 白盒类与接口视图）。
- **[architecture_v2.puml](architecture_v2.puml)**：LLM-EdgeFlow 平台目标演进全景图（To-Be Target Blueprint，涵盖控制面交付与跨层契约）。
- **[assets/architecture_class_diagram.svg](assets/architecture_class_diagram.svg)**：由 `architecture.puml` 固定版本生成的 As-Is 类图资产。
- **[assets/architecture_flow.svg](assets/architecture_flow.svg)**：由 `architecture_v2.puml` 固定版本生成的 Target 全景图资产。
- **[developer_guide.md](developer_guide.md)**：按任务选择开发入口，按需查询四层扩展边界和进阶接口。
- **[第一个自定义 Node](dev_guide/first_custom_node.md)**：从两个文本处理函数开始，完成源码生成、编译、连线和统一 Demo 运行。
- **[Node 作者的五个概念](dev_guide/custom_node_concepts.md)**：深入浅出解释类型端口、来源编号、模型绑定、Definition 和并发声明。
- **[SOLUTION_DEVELOPER_ARCHITECTURE_PLAN.md](SOLUTION_DEVELOPER_ARCHITECTURE_PLAN.md)**：面向方案开发者的用户诉求、custom Node 复用边界、降低接入门槛的实施阶段与验收标准。
- **[logging.md](logging.md)**：纯 C11 公共日志 API、等级、环境变量和接口约束。
- **[CHANGELOG.md](CHANGELOG.md)**：架构里程碑与用户可感知变更摘要。
- **[rfcs/ (RFC 需求与设计库)](rfcs/README.md)**：所有待实现需求、架构演进 RFC 设计文档及模板规范（`doc/rfcs/`）。
- **[Pipeline Studio](../tools/pipeline_studio/README.md)**：DAG 终端视图、Web 工作台、自动化 CLI 与安全边界。

---

## 快速导航

方案开发者先按任务进入：
[编排已有 Node](../tools/pipeline_studio/README.md#第一次编排) →
[开发自定义 Node](dev_guide/first_custom_node.md) →
[对接平台结构](BUSINESS_ONBOARDING.md) →
[运行当前方案](../tools/pipeline_studio/README.md#运行当前方案)。
只有遇到对应能力缺口才需要编写 Node 或 Adapter；真实业务效果另按
[可验证选择指南](VERIFIABLE_SELECTION.md)验收。

1. **[架构职责与扩展边界](architecture.md#1-架构总览)**：接入适配、流程编排、能力节点、模型执行。
2. **[物理代码 UML 类图](architecture.puml)**：精确对应当前 C++ 类的组合、继承与调用关系。
3. **[平台目标演进全景图](architecture_v2.puml)**：Control Plane（Manifest/Catalog/Validator）与 4 层平台的长远演进蓝图。
4. **[时序与数据流转](architecture.md#3-数据流转与调用时序-runtime-sequence)**：外部请求进出与内部算子/固定 Batch 推理流转。
5. **[自定义 Node 入门](dev_guide/first_custom_node.md)**：方案开发者的完整动手练习；进阶扩展见[开发者指南](developer_guide.md)。
6. **[公共日志 API](logging.md)**：C/C++ 接入、日志等级与 Demo 环境变量。
7. **[RFC 需求与设计规范](rfcs/README.md)**：所有新功能与需求的设计文档生命周期管理。
8. **[开发与交付流程](../CONTRIBUTING.md)**：任务分级、分支、RFC 阈值、验证和远程交付授权。
9. **[DAG 可视化工作台](../tools/pipeline_studio/README.md)**：终端查看、Web 编辑与草稿运行。

两个 SVG 均由 PlantUML `1.2024.7` 生成，禁止手工编辑。更新源文件后运行：

```bash
./scripts/render_architecture_diagrams.sh --generate
./scripts/render_architecture_diagrams.sh --check
```

# LLM-EdgeFlow 文档目录

本目录维护 LLM-EdgeFlow 的架构、开发、接口和 RFC 文档。项目概览与最短上手路径位于仓库根目录的 [README](../README.md)，版本演进摘要位于 [Changelog](CHANGELOG.md)。

- **[architecture.md](architecture.md)**：框架 4 层抽象架构说明书、模块详细职责、运行时时序图（Mermaid）及算子开发上手规范。
- **[architecture.puml](architecture.puml)**：框架代码库当前物理实现的精确 PlantUML 类图（As-Is 白盒类与接口视图）。
- **[architecture_v2.puml](architecture_v2.puml)**：LLM-EdgeFlow 平台目标演进全景图（To-Be Target Blueprint，涵盖控制面交付与跨层契约）。
- **[assets/architecture_class_diagram.svg](assets/architecture_class_diagram.svg)**：由 `architecture.puml` 固定版本生成的 As-Is 类图资产。
- **[assets/architecture_flow.svg](assets/architecture_flow.svg)**：由 `architecture_v2.puml` 固定版本生成的 Target 全景图资产。
- **[developer_guide.md](developer_guide.md)**：算法开发人员与平台接入人员完整研发上手指南。
- **[logging.md](logging.md)**：纯 C11 公共日志 API、等级、环境变量和接口约束。
- **[CHANGELOG.md](CHANGELOG.md)**：架构里程碑与用户可感知变更摘要。
- **[rfcs/ (RFC 需求与设计库)](rfcs/README.md)**：所有待实现需求、架构演进 RFC 设计文档及模板规范（`doc/rfcs/`）。
- **[Pipeline Studio](../tools/visualizer/README.md)**：DAG 终端视图、Web 工作台、自动化 CLI 与安全边界。

---

## 快速导航

1. **[4 层抽象架构规范](architecture.md#1-框架整体-4-层抽象架构)**：L1 平台接入、L2 管线调度、L3 通用能力节点、L4 Model / Backend。
2. **[物理代码 UML 类图](architecture.puml)**：精确对应当前 C++ 类的组合、继承与调用关系。
3. **[平台目标演进全景图](architecture_v2.puml)**：Control Plane（Manifest/Catalog/Validator）与 4 层平台的长远演进蓝图。
4. **[时序与数据流转](architecture.md#3-数据流转与调用时序-runtime-sequence)**：外部请求进出与内部算子/固定 Batch 推理流转。
5. **[新算子与新业务扩展指引](developer_guide.md)**：算法工程师 3 步快速上手模板。
6. **[公共日志 API](logging.md)**：C/C++ 接入、日志等级与 Demo 环境变量。
7. **[RFC 需求与设计规范](rfcs/README.md)**：所有新功能与需求的设计文档生命周期管理。
8. **[开发与交付流程](../CONTRIBUTING.md)**：任务分级、分支、RFC 阈值、验证和远程交付授权。
8. **[DAG 可视化工作台](../tools/visualizer/README.md)**：终端查看、Web 编辑与草稿运行。

两个 SVG 均由 PlantUML `1.2024.7` 生成，禁止手工编辑。更新源文件后运行：

```bash
./scripts/render_architecture_diagrams.sh --generate
./scripts/render_architecture_diagrams.sh --check
```

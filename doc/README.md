# Alg-SDK Framework 文档目录

本目录包含算法交付与管线框架（Alg-SDK Runtime Pipeline Framework）的详细设计、架构图与开发指引：

- **[architecture.md](architecture.md)**：框架 4 层抽象架构图、模块详细职责、运行时时序图（Mermaid）及扩展规范。
- **[platform_operator_interface_design.md](platform_operator_interface_design.md)**：公司平台 `OperatorFunc`、`.conf`、命名 I/O 与输出生命周期兼容层设计。
- **[../tools/visualizer/index.html](../tools/visualizer/index.html)**：交互式 DAG 管线与数据流向可视化 Web 工具。

---

## 快速导航

1. **[4 层抽象架构图](architecture.md#1-框架整体-4-层抽象架构)**：L1 平台接入、L2 管线调度、L3 业务算子、L4 多后端引擎。
2. **[时序与数据流转](architecture.md#3-数据流转与调用时序-runtime-sequence)**：外部请求进出与内部算子/固定 Batch 推理流转。
3. **[新算子与新业务扩展指引](architecture.md#4-算法开发者开发新业务指南3-步上手)**：算法工程师 3 步快速上手模板。
4. **[DAG 可视化工作台](../tools/visualizer/index.html)**：交互式节点状态与数据黑板流向监控。

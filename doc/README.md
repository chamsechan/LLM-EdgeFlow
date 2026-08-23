# Alg-SDK Framework 文档目录

本目录包含算法交付与管线框架（Alg-SDK Runtime Pipeline Framework）的详细设计、架构图与开发指引：

- **[architecture.md](architecture.md)**：框架 4 层抽象架构图、模块详细职责、运行时时序图（Mermaid）及扩展规范。
- **[developer_guide.md](developer_guide.md)**：算法开发人员与平台接入人员完整研发上手指南。
- **[rfcs/ (RFC 需求与设计库)](rfcs/README.md)**：所有待实现需求、架构演进 RFC 设计文档及模板规范（`doc/rfcs/`）。
- **[../tools/visualizer/index.html](../tools/visualizer/index.html)**：交互式 DAG 管线与数据流向可视化 Web 工具。

---

## 快速导航

1. **[4 层抽象架构图](architecture.md#1-框架整体-4-层抽象架构)**：L1 平台接入、L2 管线调度、L3 业务算子、L4 多后端引擎。
2. **[时序与数据流转](architecture.md#3-数据流转与调用时序-runtime-sequence)**：外部请求进出与内部算子/固定 Batch 推理流转。
3. **[新算子与新业务扩展指引](developer_guide.md)**：算法工程师 3 步快速上手模板。
4. **[RFC 需求与设计规范](rfcs/README.md)**：所有新功能与需求的设计文档生命周期管理。
5. **[DAG 可视化工作台](../tools/visualizer/index.html)**：交互式节点状态与数据黑板流向监控。

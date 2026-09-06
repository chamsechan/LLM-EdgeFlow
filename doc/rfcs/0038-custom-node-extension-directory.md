# RFC 0038: 自定义 Node 的统一源码扩展目录

- **RFC 编号**：0038-custom-node-extension-directory
- **创建日期**：2026-09-06
- **文档状态**：Completed
- **关联分支**：`feat/custom-node-extension`
- **目标版本**：v10.x
- **负责人 / 作者**：LLM-EdgeFlow Team

## 1. 背景与动机

方案开发者需要编写领域算法，也需要在不同 Pipeline 中复用这些算法。用户已明确接受
仓库内源码扩展和重新编译，要求用统一的 custom Node 目录避免按业务建立大量目录。
现有 `src/common_nodes/` 面向通用操作，把所有领域算法放入其中会模糊维护边界；要求
每个领域算法先证明无法通用化，也会增加接入成本。

## 2. 设计范围与边界

- 新增 `src/custom_nodes/`，默认按操作命名单个源码文件，与 `src/common_nodes/` 并列。
- 同步构建、分层检查、开发文档和 Agent 的 Layer 3 路由。
- 沿用既有 Node、Definition、Catalog、Validator 和模型能力接口，不增加运行时抽象。
- Adapter、Pipeline 配置、Demo 继续使用现有目录；不按业务迁移或复制这些文件。
- 本次不新增具体算法、节点脚手架生成器、动态插件、公共 ABI 或模型能力。

## 3. 总体技术方案

### 3.1 Layer 3 源码归属

`common_nodes` 与 `custom_nodes` 都通过 `target_sources` 加入
`edgeflow_layer3_node_objects`，由现有 Composition Root 链入 SDK、工具和 Demo。
新目录初始为空，仅包含构建入口与接入指南；不注册无实际需求的示例生产节点。

- `common_nodes`：经过通用化设计、由框架维护的共享操作。
- `custom_nodes`：用户编写的领域处理、特定前后处理或模型调用组合，可被多个方案复用。
- `include/nodes`：两者共享的浅层支持接口；不增加独立 CustomNode 基类。

### 3.2 编写与复用

先查询 Catalog；已有操作可满足时直接组合。缺失的领域算法可以放入 `custom_nodes`，
无需为了目录准入而把它改造成通用算子。变更风险和 RFC 阈值仍由 CONTRIBUTING 统一决定。
这取代 RFC-0012 中领域 Node 必须额外证明通用组合违反语义、原子性或性能的准入限制，
保留其类型端口、无请求状态和注册约束。

节点继承 `NodeBase` 或已有模型绑定支持类，通过
`REGISTER_NODE_WITH_DEFINITION` 注册。Definition 的 `category` 使用 `custom`；端口、
配置、模型绑定、并发能力和来源关联必须完整声明。只有真实外部契约限制才设置
`biz_names`，目录本身不限制业务可见性。具体可用节点始终查询运行时 Catalog。

Node 可以在一次处理内完成前处理、调用声明绑定的模型、后处理；平台结构转换仍由
Adapter 执行。跨节点复用优先通过 Pipeline 连线，公共辅助逻辑使用普通函数，不直接
构造或调用另一个 Node 来绕过调度。领域节点经实际复用和独立评审后可提升为 common。

## 4. 关键不变量

1. 自定义 Node 同样不得包含平台 C ABI、Operator 或 Adapter 头，不能持有平台输入指针。
2. Core、Model/Backend 和通用 Node 不得依赖自定义 Node 的实现。
3. 请求数据留在局部变量或 AlgContext；模型生命周期、模型语义与厂商 SDK 仍由 Layer 4 管理。
4. 现有 Node 注册、类型检查、输出发布与模型并发约束不因目录改变而放宽。
5. 默认构建和正式导出面不增加能力；目录分类不形成第二套 Catalog。

## 5. 测试与质量验收

- 扩展现有 LayerGuard 自测：拒绝 custom Node 包含平台头、业务 keys、厂商头；拒绝
  common/Core/Engine 对 custom 实现的依赖；检查新目录的构建归属。
- 正常工作区通过 LayerGuard，现有 Catalog、Pipeline、Adapter、Demo 与工具测试继续通过。
- 运行 `./scripts/run_all_tests.sh`，记录结果后完成 RFC。
- 此变更不涉及新的业务效果、模型或硬件，不以构建目录准备宣称具体算法已实现。

实际验证：`./scripts/run_all_tests.sh` 于 2026-09-06 通过，89/89 个 CTest 分组成功，
包括扩展后的 LayerGuard 自测、完整构建和现有 Demo/Studio 回归。开发技能结构校验和
新增接入文档链接检查通过；运行时 Catalog 与变更前一致，仍为原有 11 个生产 Node。
本次日志位于 `/tmp/edgeflow-custom-nodes-gate.log`，临时日志不是交付依赖。

## 6. 实施里程碑

1. [x] 按用户提出的统一目录方案记录决策并自评。
2. [x] 完成目录、构建、分层检查和开发入口。
3. [x] 通过统一门禁并完成验证记录。

## 7. 变更记录

| 日期 | 版本 | 变更内容 | 作者 |
| :--- | :--- | :--- | :--- |
| 2026-09-06 | v1.0.0 | 采用统一 custom Node 目录，保留现有四层与源码注册机制 | LLM-EdgeFlow Team |

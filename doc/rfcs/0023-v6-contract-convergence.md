# RFC 0023: v6 运行时契约破坏性收敛

- **RFC 编号**：0023-v6-contract-convergence
- **创建日期**：2026-08-30
- **文档状态**：Completed
- **关联分支**：`refactor/v6-contract-convergence`
- **目标版本**：v6.0.0
- **负责人 / 作者**：LLM-EdgeFlow Team

---

## 1. 背景与动机 (Motivation & Context)

仓库尚未正式接入项目，但历史迁移保留了多组双语义接口：`biz_name` 与
`business_name` 同时解析和输出、`INode` 两套 Init、`AlgContext` 的 write-once API 与
可覆盖兼容 API、以及已经不参与 Validator 决策的 `allow_override`。这些入口增加了
Catalog、CLI、测试和作者文档的状态空间，并使一次请求内重复覆盖的旧黑板值一直保留到
请求结束。

继续维护这些兼容层只会把尚未发生的外部兼容成本固化成长期架构成本。本 RFC 定义 v6
唯一契约，在正式启用前一次迁完，降低公共 C++ 接口、持久 Schema 和运行时所有权语义的
复杂度。

## 2. 设计范围与边界 (Scope & Non-Goals)

### 2.1 范围内 (In-Scope)

- [x] 持久 Pipeline 配置只接受 `biz_name`；运行时结构只保存一个 biz 标识。
- [x] Catalog JSON 升级到 schema v2，只输出 `bizs`、`biz_name`、`demo_biz` 和
  `biz_names`。
- [x] Demo Profile JSON 升级到 schema v2，只接受 `biz`。
- [x] C++ Registry/Definition API 删除 Business 类型、方法和宏别名。
- [x] CLI 只接受 `--biz` 和中性短选项 `-b`；Demo、脚本、测试与当前文档同步迁移。
- [x] `INode` 只保留 `Init(const NodeInitContext&)`，生产 Node 与测试替身统一实现。
- [x] `AlgContext` 只保留 `Publish/Read` 的 write-once 请求值契约，删除
  `Set/Get/Erase/Clear` 及覆盖快照保留机制。
- [x] 删除 `PortDefinition::allow_override` 及其构造参数和 Catalog 输出。
- [x] 保持全部官方 Pipeline 可通过 Catalog、Validator、plan 和运行时回归。

### 2.2 非目标 (Non-Goals / Out-of-Scope)

- 不修改六个导出的 `Alg_*` C ABI、异常屏障或 `CompanyAlgBizType`。
- 不改变 Pipeline 的显式 `id` / `depends_on` DAG、Node 类型、业务行为或模型协议。
- 不为尚未部署的旧 C++/JSON/CLI 契约提供弃用周期、环境开关或自动转换器。
- 不改写已归档 RFC；本 RFC 仅取代 RFC-0010、RFC-0018 中的兼容保留决策，并落实
  RFC-0020 已确定的重复生产者 fail-closed 语义。

## 3. 总体技术方案与架构设计 (Architecture & Technical Design)

### 3.1 架构分层映射 (4-Tier Mapping)

- **Layer 1**：Adapter 与 Operator 只使用 `biz_name`，通过 `Publish` 按 write-once 契约装填请求输入；
  C ABI 保持不变。
- **Layer 2**：Pipeline Config、Catalog、Session Runtime Options 和 Blackboard 采用唯一
  v6 契约；Validator 继续是端口冲突的单一判定者。
- **Layer 3**：Node 作者只实现 `NodeInitContext` 初始化入口；端口定义不再声明无效的
  覆盖许可。
- **Layer 4**：无接口或行为修改。

依赖方向保持 Layer 1 → Layer 2 → Layer 3 → Layer 4。

### 3.2 唯一接口与 Schema

```cpp
struct ParsedPipelineConfig {
    std::string biz_name;
};

class INode {
public:
    virtual bool Init(const NodeInitContext& context) = 0;
};

class AlgContext {
public:
    template <typename T> bool Publish(std::string key, T&& value);
    template <typename T> const T* Read(const std::string& key) const;
};
```

Catalog v2 不再输出同值别名。`PortDefinition` 只描述 key、类型、必需性、基数、
provenance 和生命周期；同一 key 的多生产者始终由 `PipelineValidator` 拒绝。

### 3.3 Blackboard 生命周期

Adapter 和 Node 都通过 `Publish` 首次写入；重复发布返回 `false`，现有值保持不变。
`Read` 返回的只读指针在 `AlgContext` 生命周期内稳定。由于 v6 不再允许 erase、clear 或
覆盖，容器可直接持有 `std::any`，无需为每次写入分配 `shared_ptr`，也无需保存
`retired_snapshots_`。

## 4. 关键设计考量与权衡 (Design Trade-offs & Invariants)

1. **有意破坏兼容**：旧字段、别名和参数会在编译、解析或 CLI 层直接失败，避免静默
   接受两套契约；仓内调用点在同一变更集中全部迁移。
2. **写一次语义**：请求输入和 Node 输出遵守同一 `Publish/Read` 模型；重复发布
   fail-closed，不以覆盖顺序决定结果。
3. **指针稳定性**：移除删除和覆盖后，已发布值在请求结束前不失效；并发读取与不同 key
   的发布由共享互斥保护。
4. **单一初始化上下文**：配置、Session、端口 Binding 只经 `NodeInitContext` 传递，测试
   使用测试辅助函数，不把便利重载重新引入生产接口。
5. **Schema 明确升级**：Catalog 使用整数版本 2，消费方必须显式适配，不依赖字段探测。

## 5. 测试与质量验收计划 (Testing & Verification Plan)

- [x] 配置解析测试证明只接受 `biz_name`，`business_name` fail-closed。
- [x] Catalog 契约测试证明 schema v2 不包含 Business 或 `allow_override` 别名。
- [x] Blackboard 测试覆盖首次发布、重复发布拒绝、类型错误、并发不同 key 发布和读指针
  稳定性。
- [x] Node 测试全部经 `NodeInitContext` 初始化，生产代码不存在旧 Init 签名。
- [x] CLI、Demo 与 `scripts/show.py` 测试覆盖 `--biz`；旧 `--business` 被拒绝。
- [x] 对 9 个官方配置执行 validate/plan，并完成可执行 Demo smoke。
- [x] 交付前运行一次 `./scripts/run_all_tests.sh`。

## 6. 实施路线与里程碑 (Implementation Milestones)

1. [x] **阶段一：建立并索引 RFC，确定 v6 兼容边界**。
2. [x] **阶段二：统一 biz/Catalog schema v2 和仓内消费方**。
3. [x] **阶段三：收敛 Node Init、Blackboard 与 Port Definition**。
4. [x] **阶段四：完成聚焦回归、官方 Pipeline/Demo 验证和统一质量门禁**。
5. [x] **阶段五：将 RFC 标记为 Completed；仅在用户授权后创建 PR 或合并**。

## 7. 变更记录 (Changelog)

| 日期 | 版本 | 变更内容 | 作者 |
| :--- | :--- | :--- | :--- |
| 2026-08-30 | v1.0.0 | 建立 v6 运行时唯一契约与迁移方案 | LLM-EdgeFlow Team |
| 2026-08-30 | v1.1.0 | 完成仓内迁移、聚焦验证与统一质量门禁 | LLM-EdgeFlow Team |

# RFC 0031: 业务 Blackboard Key 所有权拆分

- **RFC 编号**：0031-business-blackboard-key-ownership
- **创建日期**：2026-09-02
- **文档状态**：Completed
- **关联分支**：`refactor/external-readiness-layer-boundaries`
- **目标版本**：v10.x
- **负责人 / 作者**：LLM-EdgeFlow Team

---

## 1. 背景与动机

`core/common_contracts.h` 同时保存中立 payload/type traits 和全部业务 Pipeline key。新增业务
因此必须修改 Layer 2 中央头文件，而生产 Node 实际只依赖中立类型和逻辑端口，不依赖这些
业务 key。

## 2. 范围与非目标

范围内：

- `core/common_contracts.h` 只保留业务中立 payload、type traits 和通用控制命令。
- 现有业务 ingress/egress 与 Pipeline 绑定 key 移到 Layer 1 Adapter 所有的头文件。
- Core/Engine 测试使用测试本地 typed keys，不反向依赖 Adapter。
- LayerGuard 禁止 Layer 2～4 引入 Layer 1 业务 key 头。

非目标：不更改任何 key 字符串、类型、Pipeline JSON、Adapter 行为或 Node Definition。

## 3. 技术方案

新增 `include/adapter/biz_blackboard_keys.h`，集中保存已发布业务 Adapter 的 typed keys。
这是 Layer 1 的组合契约，不是新的 Core 全局注册表。生产 Node 继续使用 Definition 中的
逻辑端口和 Pipeline 已校验绑定。

## 4. 不变量

1. 所有现有 Blackboard key 名称和 `type_id` 完全不变。
2. Layer 2 不识别具体业务；新增业务 key 不再修改 `core/common_contracts.h`。
3. Adapter 继续通过 `BizDefinition` 声明 ingress/egress，Validator 仍从 Catalog 获取闭包。

## 5. 验证计划

- 现有 Adapter purity、全部业务 Pipeline、typed Blackboard 与 Validator 测试继续通过。
- 添加架构门禁，证明 Core、Node、Engine 不包含业务 key 头。
- 最终运行一次 `./scripts/run_all_tests.sh`。

## 6. 实施路线

1. [x] 移动 typed key 定义并更新 Layer 1。
2. [x] 清理 Core/Engine 测试对业务 key 的依赖。
3. [x] 扩展 LayerGuard 和文档。
4. [x] 完成完整质量门禁并标记 `Completed`。

## 7. 变更记录

| 日期 | 版本 | 变更内容 | 作者 |
| :--- | :--- | :--- | :--- |
| 2026-09-02 | v1.0.0 | 建立业务 Blackboard key 所有权拆分方案 | LLM-EdgeFlow Team |

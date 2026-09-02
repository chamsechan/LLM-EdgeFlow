# RFC 0030: 编译期分层边界与轻量运行时计划契约

- **RFC 编号**：0030-compile-time-layer-boundaries
- **创建日期**：2026-09-02
- **文档状态**：Completed
- **关联分支**：`refactor/external-readiness-layer-boundaries`
- **目标版本**：v10.x
- **负责人 / 作者**：LLM-EdgeFlow Team

---

## 1. 背景与动机

四层源码目前全部追加到单一 `edgeflow_runtime_objects`，CMake 无法表达各层所有权；同时
Layer 3 的 `node_support.h` 为读取端口绑定而包含完整 `pipeline_validator.h`。这会扩大重编译
范围，也使未来 Backend/Adapter 接入更容易形成非法反向依赖。

## 2. 范围与非目标

范围内：

- 为 Layer 1～4 建立独立 OBJECT targets，并由显式 Composition Root 聚合最终运行时。
- 建立向下传播的依赖 interface targets，保留现有静态注册和完整默认 Backend 集合。
- 把 `ValidatedNodePlan` 与端口绑定提取为轻量 Core 契约，Node 不再包含完整 Validator。
- 扩展 LayerGuard，使源码所有权和关键反向 include 可自动验证。

非目标：不改变 C ABI、Operator、Pipeline Schema、Node/Model/Backend Definition 或运行语义；
不实现 SDK 安装、打包和内网依赖。

## 3. 技术方案

```text
Root CMake Composition Root
  ├─ Layer 1 Adapter objects
  ├─ Layer 2 Core objects
  ├─ Layer 3 common Node objects
  ├─ Layer 4 Model/Backend objects
  └─ composition objects (shared runtime + cross-cutting log)
```

最终 `edgeflow_runtime_objects` interface 为内部工具和测试传播同一组 OBJECT；`alg_sdk`
直接聚合完全相同的对象集合。各层的 source CMake 只能向自己的 target 添加源码，根
CMake 是唯一最终组合点。

`ValidatedNodePlan`、`ResolvedPortBinding` 与 `PortDirection` 移入
`core/validated_node_plan.h`。`PipelineValidator` 仍是唯一规划实现，Pipeline 仍只消费完整
`ValidatedPipelinePlan`。

## 4. 不变量

1. 依赖方向保持 Layer 1 → Layer 2 → Layer 3 → Layer 4；组合只发生在 Composition Root。
2. 所有注册翻译单元继续进入最终 SDK，Catalog 条目数量和内容不变。
3. 六个 `Alg_*` 的异常屏障和 12 个动态导出入口不变。
4. 不建立第二套 Validator、Catalog 或运行时计划。

## 5. 验证计划

- LayerGuard 检查每层 CMake source ownership、Node 轻量计划 include 和 Composition Root。
- 构建 SDK、CLI、Demo、sharded tests，校验 Catalog 与全部 Pipeline。
- 运行 `SdkExportSurfaceTest` 和现有 C11/Operator 契约测试。
- 最终运行一次 `./scripts/run_all_tests.sh`。

## 6. 实施路线

1. [x] 提取 `ValidatedNodePlan` 轻量契约。
2. [x] 建立四层 OBJECT targets、依赖 interfaces 与 Composition Root。
3. [x] 扩展 LayerGuard 和架构文档。
4. [x] 完成完整质量门禁并标记 `Completed`。

## 7. 变更记录

| 日期 | 版本 | 变更内容 | 作者 |
| :--- | :--- | :--- | :--- |
| 2026-09-02 | v1.0.0 | 建立编译期分层与轻量计划契约方案 | LLM-EdgeFlow Team |

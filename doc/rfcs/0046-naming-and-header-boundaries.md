# RFC 0046: 接入目录、头文件边界与契约命名整理

- **RFC 编号**：0046-naming-and-header-boundaries
- **创建日期**：2026-09-10
- **文档状态**：Completed
- **关联分支**：`refactor/naming-and-header-boundaries`
- **目标版本**：v10.x
- **负责人 / 作者**：LLM-EdgeFlow Team
- **关联决策**：补充 RFC-0011、RFC-0028、RFC-0030、RFC-0045；保留 RFC-0029 的内网交付边界。

## 1. 问题与范围

基线 `2f5dbaa` 中，业务 Adapter 和 Operator bridge 分散在不同深度的目录，内部头与
源码扩展契约的放置规则不一致，公开头目标传播整个 `include/`。Adapter 的业务名称与
Pipeline 业务契约同名，端口声明混用逻辑名与黑板键，Adapter 日志还将固定 `2.0.0`
标为 ABI，与项目公共 ABI `5.0.0` 混淆。配置、测试归属及部分展示文案也存在命名漂移。

本次整理覆盖接入适配层、流程编排层、能力节点层、模型执行层的源码接口和相关工具、
示例、文档；保持算法行为、依赖方向、所有权、并发、模型配置和运行时 Pipeline Schema。

## 2. 决策与权衡

- 接入实现继续由 `src/adapter/` 持有；业务 Adapter 与 `<biz>_operator_bridge.cpp`
  相邻放入 `biz/`，`operator/` 只维护通用机制。内部运行时及部署解析头与实现相邻。
- 本项目公开头以 `edgeflow/` 标识归属；SDK 调用头、源码扩展头和内部源码头分别由
  CMake 使用范围控制。桥接描述符、注册入口与必要 I/O 值契约属于源码扩展接口；
  注册表状态、配置解析、输出池调度仍在内部实现。
- Adapter 标识使用 `adapter_name` / `AdapterName`，业务契约集合使用
  `biz_definitions`。日志中的 `sdk_abi_version` 直接使用 CMake 生成的公共 ABI 版本。
- 逻辑端口与业务黑板端口使用不同的源码类型；Catalog JSON 保持既有 `key` 字段，
  由序列化边界映射。元数据定义与 Catalog 服务分离，张量实现与推理元数据分离。
- `node_registry.h` 的主要类型命名为 `NodeRegistry`。现行日志采用四层职责名称；
  Demo 展示使用业务名称，移除手工维护的业务编号。
- 示例配置按方案前缀及部署变体命名；更新 Profile、测试和现行文档的引用。测试文件
  按实际责任迁移，继续复用现有 runner。保持 common/custom、Model/Backend、
  dev_support/tests/support 的现有职责划分。

## 3. 兼容与迁移

公共 C/C++ 函数、结构布局、`Company*` 类型、导出宏和 `libcompany_alg_sdk` 名称保持
原样；旧公共头保留转发入口，调用方可逐步采用 `edgeflow/` 路径。不新增 install/export
或预设公司内部 SDK 契约。

Catalog JSON 和运行时 Pipeline 的业务 ID、端口绑定、Model/Backend 名称保持原样。
内部 C++ 源码扩展随源码重新编译，按现行开发指南更新成员、类型及头文件位置。
示例配置路径和误导性的 Profile 名称属于开发资料迁移，提供明确映射并更新仓库内所有
活动引用；历史 RFC 和审计报告保留原始路径与结论。

## 4. 验证与完成条件

- 原有 C11、C ABI、Operator 与 SDK 导出测试通过；现有测试覆盖新旧公共头并用实际
  CMake include 参数验证 SDK 无法引用扩展或内部头。
- 端口类型检查与既有 Catalog/Validator 行为通过；Catalog 的 JSON 字段和业务 ID 不变。
- Adapter 描述符版本与生成的 SDK ABI 一致；注册完整性和输出生命周期检查通过。
- 迁移后的配置通过对应生产/测试 Catalog 校验，Demo smoke 使用新的路径与 Profile。
- 新路径及拆分头继续通过依赖检查、C11 检查、文档链接与架构图检查。
- 最终执行 `./scripts/run_all_tests.sh`。本次不改变推理算法或真实模型验收范围。

## 5. 实施与最终结果

- [x] 整理目录、公共兼容头、扩展契约和 CMake 可见范围。
- [x] 完成内部命名、定义拆分、端口语义、配置与测试迁移。
- [x] 更新现行指南、迁移映射、Changelog 与架构图。
- [x] 完成聚焦验证和统一交付门禁。

验证记录：

- 六份公开头与基线逐项比对，除 include 路径外声明保持一致；C11 兼容转发及 SDK/扩展/内部头可见性由既有契约门禁覆盖。
- 新增端口角色与 Catalog `key` 序列化测试通过，Adapter 注册、转换和生命周期聚焦测试通过。
- 默认、Kite、Whisper 工具均成功构建；全部 16 份生产配置分别通过对应工具的 `validate` 和 `plan`，JSON 内容与基线相同。
- 使用迁移后的关键词配置运行效果数据集：请求 20001–20004 全部成功，前两条命中 `SYSTEM_INIT`，后两条未命中。
- 统一交付门禁为 `./scripts/run_all_tests.sh`，完成状态以该门禁成功为准。真实模型效果及目标硬件验收不在本次结构整理范围。

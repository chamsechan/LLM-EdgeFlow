# Changelog

## Unreleased

LLM-EdgeFlow 尚未正式发布。当前产品版本标识为 **v11.0.0**，公共 **ABI major 为 9**；
它们描述当前构建与接口基线，不代表已经交付的正式 Release。

当前基线采用四层架构，通过 C++ Operator SDK 接入宿主；Pipeline JSON 描述明确的数据连接、
模型配置与接入绑定，函数式 Node 使用统一 Spec 声明端口、参数、模型能力和 Control。
现行规则见[架构设计](architecture.md)、[开发者指南](developer_guide.md)和
[配置说明](../configs/README.md)，可用能力以目标构建的 Catalog 为准。

模型效果、目标设备、内部 SDK 接入和开发者体验的验证范围见
[模型、构建与效果验收](VERIFIABLE_SELECTION.md#验收范围与发布准备)。

研发过程与变更历史通过 Git 追溯。正式发布后，本文件按发布版本记录用户可感知的能力、
契约变化及必要的迁移说明。

当前修正包括：线程池部分创建失败时完整回收线程；接入 Binding 批次上限参与运行时
限额；Model 纯配置语义在 Backend 加载前校验；vendor 编译依赖隔离至 Backend。
LLM 节点复用生成参数声明和解析，保留原有字段及默认值。公共 Operator ABI 与
Pipeline 配置格式保持不变。

开发 Skills 按业务方案规划、Adapter、Map/LLM/Batch Node、Model 和 Backend 提供独立入口，
由业务需求生成组件增补与 DAG 建议，并复用现有作者 API、Catalog 和验证流程。

构建预设、验证脚本和 Kite CI 共用 `CMakePresets.json` 中的场景参数，脚本需要 CMake 3.19+。
默认门禁沿用 `build/`；新的 sanitizer 和真实模型构建分别使用 `build/sanitizers/` 与
`build/real-models/`，旧目录可按需重建或通过 sanitizer 目录变量复用。测试源码与 runner
归属集中在 `tests/RuntimeTests.cmake`，普通测试按目录自动收集 `test_*.cpp`，自定义 Node
测试无需额外维护源码清单；保留独立测试目标、CTest 分组及必需测试清单校验。

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

接入适配层去掉重复声明。删除 `BizExposureDefinition`、`REGISTER_BIZ_EXPOSURE` 及
`IoBindingRegistry` 的 Exposure 接口，注册审计不再检查"曝光业务必须有绑定"。批次上限在绑定上
声明一次：转换器的 `max_batch_size` 默认改为 0（不设限），有效上限取绑定与两个转换器中正值的
最小值，三者显式为 0 时注册审计和部署准备报错。Binding 现默认使用框架标准批次上限 64，
只有实测确需更小值时才覆盖 `max_batch_size`。
绑定的 `input_ports` / `output_ports` 可以省略同名映射，需要完整映射的代码改用 `EffectivePortMapping`。
`ValidateDecodeRequest`、`DecodeRequestRows` 删除批次上限参数，改读 Operator 填入的
`InputDecodeOptions::max_batch_size`。Catalog 中生产转换器的 `max_batch_size` 由 64 变为 0；各业务的
有效批次上限、Pipeline 配置格式与公共 Operator ABI 不变。

开发 Skills 按业务方案规划、Adapter、Map/LLM/Batch Node、Model 和 Backend 提供独立入口，
由业务需求生成组件增补与 DAG 建议，并复用现有作者 API、Catalog 和验证流程。

构建预设、验证脚本和 Kite CI 共用 `CMakePresets.json` 中的场景参数，脚本需要 CMake 3.19+。
默认门禁沿用 `build/`；新的 sanitizer 和真实模型构建分别使用 `build/sanitizers/` 与
`build/real-models/`，旧目录可按需重建或通过 sanitizer 目录变量复用。测试源码与 runner
归属集中在 `tests/RuntimeTests.cmake`，普通测试按目录自动收集 `test_*.cpp`，自定义 Node
测试无需额外维护源码清单；保留独立测试目标、CTest 分组及必需测试清单校验。

Studio 应用表单时，未修改的数值、布尔、枚举、数组和对象默认值继续保持未配置；已有显式值原样
保留，清空或选回“默认”可移除这些字段的覆盖。字符串清空仍表示显式空字符串。Backend 参数
收进“部署高级设置”，已有显式值时自动展开。

接入层输入长度上限集中在 `biz_input_constraints.h`，接受/拒绝边界、诊断和返回码不变；输出
转换器的容量字段由 ValueType 推导，显式列出时必须与 ValueType 一致。Catalog 中
`audio_result`、`audit_result`、`doc_answer` 输出转换器的容量字段按字典序报告。

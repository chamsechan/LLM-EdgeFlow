# 源码布局与命名

文件按职责归属，头文件按使用者范围放置。架构职责与依赖方向见
[架构设计](../architecture.md)，四层职责名称、源码目录与构建目标的对照见其中的
[架构总览](../architecture.md#1-架构总览)。

## 仓库顶层目录

| 目录 | 内容 |
| --- | --- |
| `include/` | 头文件，按[使用范围](#头文件的三种使用范围)分目录；`contracts/` 是各层共用的轻量运行时契约（`edgeflow_runtime_contracts`），`platform_mock/` 是平台公共定义替身 |
| `src/` | 四层实现：`adapter/`、`core/`、`common_nodes/` 与 `custom_nodes/`、`engine/`；`cli/` 是随框架编译的命令行工具 `alg_pipeline_tool` 与 `alg_show` |
| `demo/` | 统一 Demo 程序、Demo Profile 与 Mock 方案（`fixtures/mock/`） |
| `configs/` | 示例方案的 Pipeline JSON 与部署 `.conf`，命名见[配置说明](../../configs/README.md) |
| `data/` | Demo Profile 使用的示例数据集 |
| `models/` | 模型资产清单与说明；权重文件不入库 |
| `dev_support/` | 测试用 Model/Backend 替身、Node 起步模板和可选基准，不链接进生产 SDK |
| `tests/` | 按 `unit/`、`integration/`、`contract/`、`e2e/` 分类的测试，见[测试指南](../../tests/README.md) |
| `tools/` | 开发者直接运行的工具：Pipeline Studio、Node 脚手架、开发 Recipe 与模型选择检查 |
| `scripts/` | 构建、门禁、测试、格式化、架构图渲染与交付脚本 |
| `cmake_ext/` | 见[构建扩展目录](#构建扩展目录) |
| `doc/` | 架构、开发指南与参考文档，入口见[文档目录](../README.md) |
| `plans/` | 跨多个 PR、仍在进行中的工作计划；不是现行规则，最后一个阶段合入后删除，见[工作计划](../../plans/README.md) |
| `.agents/skills/` | 项目开发 Skills，用法见[文档目录](../README.md#使用开发-skills) |

新增工具按调用方选择目录：开发者在编排或编写组件时手动运行的放 `tools/`；构建、门禁
与 CI 调用的放 `scripts/`；需要链接框架运行时的 C++ 命令行程序放 `src/cli/`，可执行文件
仍输出到 `build/`。

## 构建扩展目录

本项目维护的 CMake 模块、生成模板和 Node 契约清单统一放在仓库根目录的
`cmake_ext/`，由顶层 `CMakeLists.txt` 和相应测试引用。根目录的 `cmake/` 留给公司
内部构建系统使用；本仓库不在该位置保留转发目录或符号链接。
`cmake` 命令、`CMakeLists.txt` 文件名，以及第三方安装包的 `lib/cmake/` 路径保持原约定。

## 头文件的三种使用范围

| 使用者 | 位置与构建目标 | 约定 |
| --- | --- | --- |
| SDK 调用方 | `include/edgeflow/`；`edgeflow_public_headers` | Operator、日志及生成的版本头；只有明确列举的调用头向 SDK 消费方传播 |
| 源码扩展开发者 | `include/adapter/`、`include/core/`、`include/nodes/`、`include/engine/`、`include/contracts/`；`edgeflow_extension_headers` | Adapter、Node、Model、Backend 的源码接口与共享契约，需要随框架重新编译，不承诺内部 C++ 动态 ABI |
| 模块实现与仓库测试 | `src/` 中与 `.cpp` 相邻；`edgeflow_internal_headers` | 运行时装配、配置解析、注册表内部及输出池等实现 |

运行时四层仍使用各自受限的 include view，不链接覆盖整个仓库的内部头目标。
模板、内联函数和扩展基类可以直接实现在头文件中；是否需要头文件由使用范围决定。
多个 `.cpp` 或测试需要引用一个声明，不意味着它应成为公开 SDK 接口。

使用范围之外，还要区分定义的来源：`include/platform_mock/` 存放当前外网环境使用的
平台公共定义替身，包括业务 DTO、平台枚举、控制参数、命名 I/O、函数表类型和错误码。
它们作为现行调用接口的依赖，被显式列入 SDK 头视图，但不属于框架自有数据模型。
具体清单与排除项见[平台模拟定义](../../include/platform_mock/README.md)。

## 接入适配层

```text
include/edgeflow/                 SDK 调用接口
  export.h / log.h
  operator/interface.h
  operator/types.h                Company* 数据 DTO 门面
include/platform_mock/            本地平台公共定义模拟
  error_codes.h
  operator_data_types.h / operator_types.h
include/adapter/                  源码扩展契约与辅助接口
  io_binding.h
  io_converter.h
  io_binding_registry.h
  io_converter_registry.h
  operator_value_type.h
src/adapter/
  shared_algorithm_runtime.cpp/.h
  deployment_io_config.cpp/.h
  io_binding_resolver.cpp/.h
  input/                          各业务输入转换器
  output/                         各业务输出转换器
  biz/                            各业务 I/O 绑定声明
  operator/                       Operator 通用机制
    operator_config_resolver.cpp/.h
    operator_process_binding.cpp/.h
    operator_adapter.cpp
    operator_value_type.cpp       通用值类型分配、预算与重置
```

转换器作者包含 `adapter/io_converter.h` 与 `adapter/converter_authoring.h`，编写 `InputConverter` 与
`OutputConverter` 函数回调及各自的 Definition，并通过 `REGISTER_INPUT_CONVERTER` 和 `REGISTER_OUTPUT_CONVERTER` 注册。
常见单槽、每请求一行的回调使用 `DecodeRequestRows` / `EncodeResultRows` 调用普通业务函数，
批次与绑定归辅助层；多槽、展开和汇聚保留显式算法。
各业务接入绑定在 `src/adapter/biz/` 中声明 `IoBindingDefinition`，通过 `REGISTER_IO_BINDING` 注册。
端口 Definition 与回调共用同一 typed 端口常量，端口名即业务键名，
绑定不做改名。常见必需槽可用 `ExternalInputSlot<T>` /
`ExternalOutputSlot<T>` 推导类型和默认同名后缀，输出容量字段由已注册 ValueType 决定；
特殊布局仍使用完整定义。
业务专属实现可按修改关联同文件组织，共享 converter 保留独立引用；不要求为每个业务创建聚合宏或新注册表。
宿主值类型与命名输出分配方案通过 `adapter/operator_value_type.h` 登记；实现只管理
单份结构及嵌套存储，队列、租约和初始化审计归通用机制所有。常见类型直接使用
`MakeTypedInputBinding<T>` 与 `MakePooledOutputBinding<T>`；模板保留在扩展头中，
非模板分配实现归 `src/adapter/operator/`，不按业务复制池机制。
完整步骤见[业务接入](business_onboarding.md)。

## 流程编排层

```text
include/core/                     编排契约与 Node 注册接口
  pipeline_config.h               Pipeline JSON 解析结果
  pipeline_validator.h            依赖推导与校验，产出 ValidatedPipelinePlan
  pipeline.h                      按校验计划执行
  alg_context.h / blackboard_key.h / session_context.h
  node_interface.h / node_definition.h / node_registry.h / port_definition.h
  biz_definition.h / pipeline_catalog.h
src/core/                         上述接口的实现；pipeline_config_structure.cpp/.h 等私有头相邻放置
```

## 能力节点层

```text
include/nodes/                    Node 作者接口，模板与内联实现，没有对应的 src/nodes/
  authoring.h                     Node 作者统一包含的入口头
  function_node.h                 Spec 声明、AuthorNode 与 REGISTER_FUNCTION_NODE
  node_base.h                     Node 运行时基类 NodeBase
  model_binding.h / model_calls.h / control_authoring.h
  traceable_batch_operations.h    Join、Group 等批处理与来源追踪辅助
include/contracts/parameters.h    参数声明 Parameters / Field / ConfigParser，由 authoring.h 带入
src/common_nodes/                 框架维护的中性 Node，每个文件一个 *_node.cpp
  support/                        多个 Node 共用的私有辅助
src/custom_nodes/                 领域算法 Node，按操作而非业务命名
```

起步模板在 `dev_support/node_authoring/`，由 `tools/scaffold_custom_node.py` 生成到
`src/custom_nodes/`，见[自定义 Node 源码指南](../../src/custom_nodes/README.md)。

## 模型执行层

```text
include/engine/                   Model/Backend 接口、注册表与 FixedBatchExecutor
src/engine/
  runtime/                        Model/Backend 注册表与运行时工厂
  models/<模型>/                  模型预处理与语义；共享辅助放在 common/、bge_common/
  backends/<运行时>/              厂商运行时资源，厂商头文件只在此处包含
  text/ / text_generation/        UTF-8 处理与通用自回归生成
```

## 标识符与定义

| 名称 | 含义 |
| --- | --- |
| `InputConverterDefinition.converter_id` / `OutputConverterDefinition.converter_id` | 独立输入、输出转换器标识 |
| `BizDefinition.biz_name` | 业务 ID，例如 `doc_qa`；每个业务只有一个 `IoBindingDefinition`，同样以它标识 |
| `NodePortDefinition.logical_name` | Node 的逻辑端口名称，由 Pipeline 映射到具体黑板键 |
| `BizPortDefinition.blackboard_key` | 业务 ingress/egress 使用的实际黑板键 |

普通配置只在 `deployment.io.io_binding` 填写业务名；框架沿该业务的绑定获得业务边界。
框架沿注册关系选择转换器和槽位，不按名字拼写推导载体类型。

一个业务只使用一个 `snake_case` 词根，按 I/O 契约的实际语义命名，例如 `ocr_invoice_qa`：

| 位置 | 形式 | 示例 |
| --- | --- | --- |
| `biz_name`、`deployment.io.io_binding`、`REGISTER_DEMO_BIZ` | `<词根>` | `dialogue_audit` |
| 绑定源码与 Demo 源码 | `src/adapter/biz/<词根>_bindings.cpp`、`demo/biz/<词根>_demo.cpp` | `dialogue_audit_bindings.cpp` |
| 方案、数据集与 Profile | `pipeline_<词根>_<变体>`、`corpus_<词根>`、`<词根>_<变体>` | `pipeline_dialogue_audit_kite.json` |
| 中文名 | `BizDefinition.display_name`，Demo 标题使用同一名称 | 对话合规审核 |

转换器按数据形态命名，例如 `text.plain`，可被多个业务复用，不使用业务词根。
业务名与转换器 ID 都不带版本号：发布前直接改名，发布后的不兼容变化见
[CONTRIBUTING](../../CONTRIBUTING.md#3-design-and-current-contracts)。

外部槽名在所属转换器 `.cpp` 内声明一次，回调与 Definition 复用；仅用一次的 schema ID 保持原位。

业务端口使用 `RequiredBizInput`、`OptionalBizInput`、`BizOutput`；Node 端口使用
`RequiredInputPort`、`OptionalInputPort`、`OutputPort`。两种端口类型不可相互隐式转换。
Catalog JSON 在两种端口声明中输出 `key`，由所属集合表达逻辑端口或业务黑板键。

`core/port_definition.h`、`core/node_definition.h`、`core/biz_definition.h` 分别维护
端口、Node 和业务元数据，Catalog 服务在 `core/pipeline_catalog.h`。
`engine/inference_definition.h` 维护 Model/Backend 元数据；张量与 Host 内存辅助接口
在 `engine/tensor.h`。`node_registry.h` 的主要类型是 `NodeRegistry`。

## 公共头与统一入口

SDK 调用代码使用以下 `edgeflow/` 前缀入口：

| 规范入口 | 说明 |
| --- | --- |
| `edgeflow/export.h` | 符号可见性宏 |
| `edgeflow/log.h` | 统一日志入口 |
| `edgeflow/version.h` | 版本头（由 CMake 生成） |
| `edgeflow/operator/interface.h` | C++ Operator 接口及函数表 |
| `edgeflow/operator/types.h` | `Company*` 数据 DTO 门面（转发至 `platform_mock/operator_data_types.h`） |

`Company*`、公共宏、C++ Operator 公开函数签名、结构布局及 `libcompany_alg_sdk`
名称属于调用契约。内部扩展统一使用 `NodeRegistry`。

`edgeflow/operator/types.h` 仅转发 `platform_mock/operator_data_types.h` 中的 `Company*`
数据 DTO。计算平台、Control 参数、命名 I/O 与函数表类型定义在
`platform_mock/operator_types.h`，由 `edgeflow/operator/interface.h` 引入；错误码来自
`platform_mock/error_codes.h`。
真实公司公共头需要在授权内网单独核对和接入。

示例配置和 Profile 的命名见[配置说明](../../configs/README.md)。

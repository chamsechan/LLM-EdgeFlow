# 源码布局与命名

文件按职责归属，头文件按使用者范围放置。架构职责与依赖方向见
[架构设计](../architecture.md)，本次迁移决策见 [RFC-0046](../rfcs/0046-naming-and-header-boundaries.md)。

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
具体清单与排除项见[平台模拟定义](../../include/platform_mock/README.md)，决策见
[RFC-0047](../rfcs/0047-platform-mock-header-isolation.md)。

## 接入适配层

```text
include/edgeflow/                 SDK 调用接口
  export.h / log.h
  operator/interface.h
  operator/types.h                平台交互类型门面
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
```

转换器作者包含 `adapter/io_converter.h` 与 `adapter/converter_authoring.h`，实现 `InputConverter` 与
`OutputConverter` 纯虚类，并通过 `REGISTER_INPUT_CONVERTER` 和 `REGISTER_OUTPUT_CONVERTER` 注册。
各业务接入绑定在 `src/adapter/biz/` 中声明 `IoBindingDefinition`，通过 `REGISTER_IO_BINDING` 注册。
宿主值类型与命名输出分配方案通过 `adapter/operator_value_type.h` 登记；实现只管理
单份结构及嵌套存储，队列、租约和初始化审计归通用机制所有。
完整步骤见[业务接入](business_onboarding.md)。

## 标识符与定义

| 名称 | 含义 |
| --- | --- |
| `InputConverterDefinition.converter_id` / `OutputConverterDefinition.converter_id` | 独立输入、输出转换器标识 |
| `IoBindingDefinition.binding_id` | 连接业务、转换器及外部逻辑槽位的接入绑定 |
| `BizDefinition.biz_name` | Pipeline 绑定的契约 ID，例如 `smart_doc_qa_v1` |
| `BizDefinition.demo_biz` | Demo 入口，例如 `doc_qa` |
| `NodePortDefinition.logical_name` | Node 的逻辑端口名称，由 Pipeline 映射到具体黑板键 |
| `BizPortDefinition.blackboard_key` | 业务 ingress/egress 使用的实际黑板键 |

业务端口使用 `RequiredBizInput`、`OptionalBizInput`、`BizOutput`；Node 端口使用
`RequiredInputPort`、`OptionalInputPort`、`OutputPort`。两种端口类型不可相互隐式转换。
Catalog JSON 在两种端口声明中输出 `key`，由所属集合表达逻辑端口或业务黑板键。

`core/port_definition.h`、`core/node_definition.h`、`core/biz_definition.h` 分别维护
端口、Node 和业务元数据，Catalog 服务在 `core/pipeline_catalog.h`。
`engine/inference_definition.h` 维护 Model/Backend 元数据；张量与 Host 内存辅助接口
在 `engine/tensor.h`。`node_registry.h` 的主要类型是 `NodeRegistry`。

## 公共头与统一入口

仓内代码统一使用 `edgeflow/` 前缀入口；历史转发头与别名已清理：

| 规范入口 | 说明 |
| --- | --- |
| `edgeflow/export.h` | 符号可见性宏 |
| `edgeflow/log.h` | 统一日志入口 |
| `edgeflow/version.h` | 版本头（由 CMake 生成） |
| `edgeflow/operator/interface.h` | C++ Operator 接口及函数表 |
| `edgeflow/operator/types.h` | Operator 平台交互类型门面（转发至 platform_mock） |

`Company*`、公共宏、C++ Operator 公开函数签名、结构布局及 `libcompany_alg_sdk`
名称保持原样；这些名称属于既有调用契约。内部扩展统一使用 `NodeRegistry`。

`edgeflow/operator/types.h` 转发到 `platform_mock/operator_data_types.h`，
Operator 平台交互类型集中在 `platform_mock/operator_types.h`，错误码来自 `platform_mock/error_codes.h`。
真实公司公共头需要在授权内网单独核对和接入。

示例配置和 Profile 的命名见[配置说明](../../configs/README.md)。

`common_nodes/custom_nodes`、`models/backends`、`dev_support/tests/support` 的现有
职责划分继续适用。历史 RFC 和审计报告保留当时的名称与路径。

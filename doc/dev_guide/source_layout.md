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
| SDK 调用方 | `include/edgeflow/`；`edgeflow_public_headers` | C ABI、Operator、日志及生成的版本头；只有明确列举的调用头向 SDK 消费方传播 |
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
  c_api.h / c_api.hpp
  export.h / log.h
  operator/interface.h
  operator/types.h                兼容转发
include/platform_mock/            本地平台公共定义模拟
  alg_types.h / error_codes.h
  operator_data_types.h / operator_types.h
include/adapter/                  源码扩展契约与辅助接口
  biz_adapter_interface.h
  biz_adapter_registry.h
  operator_biz_bridge.h
  operator_io_contracts.h
  operator_value_type.h
src/adapter/
  c_api_adapter.cpp
  shared_algorithm_runtime.cpp/.h
  deployment_model_resolver.cpp/.h
  biz_adapter_registry.cpp
  biz/                            每个业务的两个转换文件相邻
    doc_qa_adapter.cpp
    doc_qa_operator_bridge.cpp
  operator/                       Operator 通用机制
    operator_config_resolver.cpp/.h
    json_output_config_reader.cpp/.h
    operator_biz_bridge_registry.cpp/.h
    operator_output_pool.cpp/.h
```

Bridge 作者包含 `adapter/operator_biz_bridge.h`，使用 `MakeSingleSlotBizBridge`、
`RegisterOperatorBizBridge`、`CopyToOperatorString` 和 `REGISTER_OPERATOR_BIZ_BRIDGE`。
注册函数不接收注册表实例；描述符与转换代码无需包含内部注册表或输出池头。
新宿主值类型与命名输出分配方案通过 `adapter/operator_value_type.h` 登记；实现只管理
单份结构及嵌套存储，队列、租约和初始化审计归通用机制所有。
配置读取接口 `adapter/operator_output_config.h` 使用固定枚举及字符串，不暴露 JSON；
结构体作者用 `MakeOutputParameterParser<T>` 登记普通参数结构的解析，无需编写读取器。
完整步骤见[业务接入](business_onboarding.md)。

## 标识符与定义

| 名称 | 含义 |
| --- | --- |
| `AdapterName()` / `AdapterDescriptor.adapter_name` | Adapter 标识，例如 `DocQA` |
| `AdapterDescriptor.biz_definitions` | Adapter 支持的业务 I/O 契约集合，不是 Pipeline 实例 |
| `BizDefinition.biz_name` | Pipeline 绑定的契约 ID，例如 `smart_doc_qa_v1` |
| `BizDefinition.demo_biz` | Demo 入口，例如 `doc_qa` |
| `NodePortDefinition.logical_name` | Node 的逻辑端口名称，由 Pipeline 映射到具体黑板键 |
| `BizPortDefinition.blackboard_key` | 业务 ingress/egress 使用的实际黑板键 |
| `AdapterDescriptor.sdk_abi_version` | 与生成的 `COMPANY_ALG_ABI_VERSION` 一致的公共 SDK ABI |

业务端口使用 `RequiredBizInput`、`OptionalBizInput`、`BizOutput`；Node 端口使用
`RequiredInputPort`、`OptionalInputPort`、`OutputPort`。两种端口类型不可相互隐式转换。
Catalog JSON 为兼容现有消费者，继续在两种声明中输出 `key`，由所属集合表达角色。

`core/port_definition.h`、`core/node_definition.h`、`core/biz_definition.h` 分别维护
端口、Node 和业务元数据，Catalog 服务在 `core/pipeline_catalog.h`。
`engine/inference_definition.h` 维护 Model/Backend 元数据；张量与 Host 内存辅助接口
在 `engine/tensor.h`。`node_registry.h` 的主要类型是 `NodeRegistry`。

## 兼容迁移

新代码使用以下头路径；旧公共头继续转发到同一声明，新旧头可以同时包含。

| 旧路径 | 当前路径 |
| --- | --- |
| `company_alg_interface.h` | `edgeflow/c_api.h` |
| `company_alg_cpp.hpp` | `edgeflow/c_api.hpp` |
| `company_alg_export.h` | `edgeflow/export.h` |
| `company_alg_log.h` | `edgeflow/log.h` |
| `company_alg_version.h` | `edgeflow/version.h`（由 CMake 生成） |
| `operator/operator_interface.h` | `edgeflow/operator/interface.h` |
| `operator/company_operator_types.h` | `edgeflow/operator/types.h` |

`Alg_*`、`Company*`、公共宏、C/C++ 公开函数签名、结构布局及 `libcompany_alg_sdk`
名称保持原样；这些名称属于既有调用契约。内部扩展应更新 `BizName()`、`pipelines`、
`abi_version` 等旧成员，使用上表中的明确命名。`NodeFactory` 保留源码别名，新增代码
使用 `NodeRegistry`。业务 bridge 转为无参注册函数，并使用上述扩展入口。

`edgeflow/c_api.h` 的参数类型来自 `platform_mock/alg_types.h`，错误码来自
`platform_mock/error_codes.h`；`edgeflow/operator/types.h` 转发到
`platform_mock/operator_data_types.h`，Operator 平台交互类型集中在
`platform_mock/operator_types.h`。现有兼容范围是本仓库的调用约定，真实公司公共头
需要在授权内网单独核对和接入。

示例配置与 Profile 的旧新名称见[配置迁移表](../../configs/README.md)。Pipeline JSON
业务 ID、节点类型、模型和 Backend ID、端口绑定以及算法参数均保持原样。

`common_nodes/custom_nodes`、`models/backends`、`dev_support/tests/support` 的现有
职责划分继续适用。历史 RFC 和审计报告保留当时的名称与路径。

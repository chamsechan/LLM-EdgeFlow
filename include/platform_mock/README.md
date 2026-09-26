# 本地平台公共定义模拟

此目录保存当前外网环境为跑通本项目、统一 Demo 和测试而使用的**平台公共定义替身**。
类型名、枚举值和结构布局只表示本仓库当前约定，不证明它们与真实公司公共头兼容，
也不代表公司内部 SDK 的真实接口。

| 文件 | 现有模拟定义 |
| --- | --- |
| [error_codes.h](error_codes.h) | `COMPANY_ALG_SUCCESS` 和 `COMPANY_ALG_ERR_*` 错误码 |
| [operator_data_types.h](operator_data_types.h) | `CompanyString`、`CompanyBuffer`、`CompanyAny`、`CompanyFrame`、`CompanyOdOutput` 及各业务 `CompanyOperator*` 聚合结构 |
| [operator_types.h](operator_types.h) | C++ 平台枚举 `ComputePlatform`、控制命令/参数、`CreateParam`、`OpaqueData`、`NamedIo`、`NamedIoBatch` 和 `OperatorFunc` 函数表类型 |

`error_codes.h` 可用于 C；`operator_data_types.h` 与 `operator_types.h` 为 Operator 平台结构与 C++17 交互类型头。
这些文件只持有模拟平台声明，不实现算法、平台资源或硬件能力。平台枚举存在不代表
对应芯片已有可用 Backend。

## 框架入口与包含方式

SDK 调用方包含 `edgeflow/operator/interface.h`，获得函数入口以及 `CreateParam`、
`NamedIoBatch`、`OperatorFunc` 等交互类型。`edgeflow/operator/types.h` 只转发
`operator_data_types.h` 中的 `Company*` 数据结构。
只需要平台数据结构的 Demo/接入代码可包含本目录对应头。
当前公开 CMake 头视图显式列出这些依赖；Core、Nodes、Models 和 Backends 不可包含它们。

以下内容有各自的真实实现职责，不属于平台公共定义模拟：

- `edgeflow/log.h`、`export.h`、生成的版本头：本项目日志、符号导出和版本接口。
- `adapter/biz_results.h`、业务 Blackboard keys、`operator_io_contracts.h`：框架内部结果、业务端口和池容量契约。
- `contracts/`、`core/`、`engine/` 中的中性类型：框架与模型执行协议。
- `demo/common/dataset_reader.h`、`result_writer.h`：Demo 数据集读取、输出记录和统计结构。

## 进入内网后

完整项目进入授权内网后，才能核对真实公共头、替换接入边界的模拟依赖，并验证枚举、
布局、所有权、控制和 I/O 转换。外部工作区不得请求、推断、复制或提交内部 SDK 的头文件、
库、模型、配置和凭据；这里只准备中立接入边界。
验收范围见[模型、构建与效果验收](../../doc/VERIFIABLE_SELECTION.md#验收范围与发布准备)。

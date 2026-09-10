# 本地平台公共定义模拟

此目录保存当前外网环境为跑通本项目、统一 Demo 和测试而使用的**平台公共定义替身**。
内容从仓库现有声明原样迁移，未读取或复刻公司内部 SDK；类型名、枚举值和结构布局只
表示本仓库当前约定，不证明它们与真实公司公共头兼容。

| 文件 | 现有模拟定义 |
| --- | --- |
| [alg_types.h](alg_types.h) | `CompanyAlgBizType`、C ABI 创建/控制参数，以及问答、关键词、实体、风控、OCR、语音、精排的输入输出 DTO |
| [error_codes.h](error_codes.h) | `COMPANY_ALG_SUCCESS` 和 `COMPANY_ALG_ERR_*` 错误码 |
| [operator_data_types.h](operator_data_types.h) | `CompanyString`、`CompanyBuffer`、`CompanyAny`、`CompanyFrame`、`CompanyOdOutput` 及各业务 `CompanyOperator*` 聚合结构 |
| [operator_types.h](operator_types.h) | C++ 平台枚举 `ComputePlatform`、控制命令/参数、`CreateParam`、`OpaqueData`、`NamedIo`、`NamedIoBatch` 和 `OperatorFunc` 函数表类型 |

前三个头可独立用于 C11；`operator_types.h` 是 C++17 交互类型头。
这些文件只持有模拟平台声明，不实现算法、平台资源或硬件能力。平台枚举存在不代表
对应芯片已有可用 Backend。

## 框架入口与包含方式

SDK 调用方仍包含 `edgeflow/c_api.h` 或 `edgeflow/operator/interface.h`。
只需要平台数据结构的 Demo/接入代码可包含本目录对应头。
`edgeflow/operator/types.h` 和更早的公共头路径保留转发，避免重复声明。
当前公开 CMake 头视图显式列出这些依赖；Core、Nodes、Models 和 Backends 不可包含它们。

以下内容有各自的真实实现职责，不属于平台公共定义模拟：

- `edgeflow/log.h`、`export.h`、生成的版本头：本项目日志、符号导出和版本接口。
- `adapter/biz_results.h`、业务 Blackboard keys、`operator_io_contracts.h`：框架内部结果、业务端口和池容量契约。
- `contracts/`、`core/`、`engine/` 中的中性类型：框架与模型执行协议。
- `demo/common/dataset_reader.h`、`result_writer.h`：Demo 数据集读取、输出记录和统计结构。

## 进入内网后

依照 [RFC-0029](../../doc/rfcs/0029-external-readiness-and-intranet-sdk-migration.md)，
在授权环境中核对真实公共头，替换接入边界的模拟依赖，并验证枚举、布局、所有权、
控制和 I/O 转换。不要把真实公司头复制到这个外网目录，也不要根据此目录猜测其接口。
本次隔离记录见 [RFC-0047](../../doc/rfcs/0047-platform-mock-header-isolation.md)。

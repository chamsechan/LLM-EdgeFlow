# 本地平台公共定义模拟

此目录保存当前外网环境为跑通本项目、统一 Demo 和测试而使用的**平台公共定义替身**。
类型名、枚举值和结构布局只表示本仓库当前约定，不证明它们与真实公司公共头兼容，
也不代表公司内部 SDK 的真实接口。

| 文件 | 现有模拟定义 |
| --- | --- |
| [error_codes.h](error_codes.h) | `COMPANY_ALG_SUCCESS` 和 `COMPANY_ALG_ERR_*` 错误码 |
| [log.h](log.h) | 日志等级、set/get 声明及供外网开发使用的简化打印宏 |
| [operator_data_types.h](operator_data_types.h) | 文本输入 envelope `MockInput` / `MockDataPool` / `MockTextInput`，统一输出 `MockOutput` / `MockTextOutput`，以及 `MockString`、`MockAny`、`MockFrameInput` 和其他业务输入 `Mock*Input` |
| [operator_types.h](operator_types.h) | C++ 平台枚举 `ComputePlatform`、控制命令/参数、`CreateParam`、`OpaqueData`、`NamedIo`、`NamedIoBatch` 和 `OperatorFunc` 函数表类型 |

`error_codes.h` 与 `operator_data_types.h` 可用于 C；`operator_types.h` 提供 C++17 交互类型。
数据与 Operator 头只持有模拟平台声明，不实现算法、平台资源或硬件能力。
`log.h` 另提供兼容 C11 的基本 stdio 打印替身；本地等级函数由 `src/log.cpp` 提供。
平台枚举存在不代表对应芯片已有可用 Backend。

mock 输入输出载体不包含请求 ID 字段。`Process` 以输入输出 vector 的同一行关联请求；
图像帧序号也不参与关联。

文本输入（`keyword_in`、`entity_in`）使用嵌套 envelope：
`MockInput{type=kMockInputMultidata, num_frame=1}` → `MockDataPool{data_type=kMockDataText}`
→ `MockTextInput{value, length}`。每层 `byte_size` 必须等于该层 `sizeof`；binding 依次校验
root、data pool、文本长度，再复制到中立 `TextInputValue`。文本 envelope 不带 `service_type`，
其他业务输入仍带 `service_type`。

现有业务共用输出 `MockOutput{type=kMockOutputOc, service_type, text}`：`text` 指向
`MockTextOutput`，承载完整 JSON 响应。root、嵌套文本和 `result_json_max_bytes + 1` 字节缓冲区
同属一个池化 block；容量只来自外部参数，不出现在结构中。复用前清空 `length` 与首字节，
写入超容量即失败，不写入部分内容；一次 `Process` 的所有输出全部成功后统一发布，任一失败则全部
租约回滚、所有输出保持 nullptr。输出结构不暴露状态码。
Pipeline 使用同一 `json_out` 类型和不同业务 `name`，由各输出转换器组装 JSON。
未来需要不同结构时，仍通过独立的 ValueType/binding 和转换器注册，不向中央分发增加业务分支。

`NamedIo` 的 key 是宿主路由协议，固定为 `<prefix>.<type>`：prefix 由宿主生成，非空且不含 `.`，
Runtime 只校验格式，不解读也不合成；同一方向内每种 type 至多选用一次。

## 框架入口与包含方式

SDK 调用方包含 `edgeflow/operator/interface.h`，获得函数入口以及 `CreateParam`、
`NamedIoBatch`、`OperatorFunc` 等交互类型。`edgeflow/operator/types.h` 只转发
`operator_data_types.h` 中的 `Mock*` 数据结构。
只需要平台数据结构的 Demo/接入代码可包含本目录对应头。
当前公开 CMake 头视图显式列出这些依赖；Core、Nodes、Models 和 Backends 不可包含平台业务头。
`log.h` 是共享日志基础设施的精确例外，由 `edgeflow/log.h` 转发并进入共享契约头视图；
框架和 Demo 统一包含 `edgeflow/log.h`，不直接包含日志替身头。

以下内容有各自的真实实现职责，不属于平台公共定义模拟：

- `edgeflow/log.h`：日志转发入口；`edgeflow/export.h`：本项目符号导出接口。
- 业务 Blackboard keys、`operator_io_contracts.h`：业务端口和池容量契约。
- `contracts/`、`core/`、`engine/` 中的中性类型：框架与模型执行协议。
- `demo/common/dataset_reader.h`、`result_writer.h`：Demo 数据集读取、输出记录和统计结构。

## 进入内网后

完整项目进入授权内网后，才能核对真实公共头、替换接入边界的模拟依赖，并验证枚举、
布局、所有权、控制和 I/O 转换。日志宏由正式平台提供，届时替换日志转发入口和本地等级实现，
核对 `AlgBase_setLogLevelByName` 的 `0/-1` 返回约定；本地输出格式不构成平台契约。
当前除文本 envelope 外的 `Mock*Input`、`MockFrameInput`
和 `MockOutput` 均带 `service_type`；`kMockService*` 是外网模拟的占位枚举，不表示公司内部实际取值。
这些成员与枚举、帧及扩展输出布局使用的 `MockAny` 元数据、槽声明的固定 `metadata_count` /
`metadata_type_id` 和外部 map key 后缀（`type`）都须按真实头文件与宿主约定核对。
各载体不带请求 ID；请求 ID 和业务成员由平台登记显式声明。外部工作区不得请求、推断、复制或提交内部 SDK 的头文件、
库、模型、配置和凭据；这里只准备中立接入边界。
验收范围见[模型、构建与效果验收](../../doc/VERIFIABLE_SELECTION.md#验收范围与发布准备)。

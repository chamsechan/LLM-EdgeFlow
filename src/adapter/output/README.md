# Output Converters (输出转换器)

本目录的输出转换器读取 Pipeline 结果，并组装完整业务响应。

当前八类业务都登记为 `json_out/<业务名>`，使用中立值 `JsonOutputValue`。
转换器把完整结果序列化为 `result_json`，binding 写入统一 C 结构 `MockOutput` 的嵌套文本。
`JsonOutputValue` 只承载完整 JSON 文本；对象、数组和数值保持 JSON 类型。
容量参数通过 `adapter/json_output_parameters.h` 的 `JsonOutputParameters()` 统一声明为
`result_json_max_bytes`，默认 8191 字节，硬上限 65536 字节。
如果新业务复用该载体，新增转换器和 binding 业务枚举映射。
如果需要新结构，通过 ValueType 注册。

## 规范与契约

- 通过 `REGISTER_OUTPUT_CONVERTER` 注册 `OutputConverterDefinition`，槽用 `ExternalOutputSlot<Value>` 声明 `adapter/io_values.h` 中的中立值类型。
- 如果每个请求只有一个槽和一个结果，使用 `EncodeResultRows<Value>`。
  业务函数接收结果和 `Value*`，并设置字符串、数组或标量。
  框架按原始批内行号排序，调用 binding 写入，并更新 `written_count`。
- 多路结果组合显式使用 `ReadOutputValue` / `IndexResults`，组装中立值后调用 `WriteOutputValue`。可选输出跳过缺失行并保留原始行号。
- Definition 与回调共用类型端口，实际 key 通过 `options.Port(逻辑端口)` 读取。Converter 校验结果基数、来源与业务状态，组装 JSON 等完整响应。
- Node 只发布中立结果。外部字段名、默认值和序列化格式在此生成。共享序列化函数放在本目录私有头中，例如 `rule_match_response.h`。
- binding 负责平台成员、业务枚举、字符串表示、metadata 与输出布局。按 view 中真实池规格检查容量并复制，不从字符串内容长度推断容量，不静默截断或忽略内嵌 NUL。
- 写入失败时，租用块的部分内容可能已改变。Operator 只在全部输出成功后发布结果。
  任一输出失败时，Operator 归还全部租约。Converter 不保留宿主指针，也不将请求局部指针写入输出。

平台 binding 的注册与所有权规则见[输出分配指南](../../../doc/dev_guide/operator_output_allocation.md)。

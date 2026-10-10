# Output Converters (输出转换器)

本目录包含内部 Pipeline Blackboard 结果到完整外部响应的业务转换器。

## 规范与契约

- 通过 `REGISTER_OUTPUT_CONVERTER` 注册 `OutputConverterDefinition`，槽用 `ExternalOutputSlot<Value>` 声明 `adapter/io_values.h` 中的中立值类型。
- 单槽、每请求一个结果使用 `EncodeResultRows<Value>`；业务函数接收结果与 `Value*`，直接赋值字符串、数组与业务标量。框架负责按原始批内行号重排、binding 写入及 `written_count`。
- 多路结果组合显式使用 `ReadOutputValue` / `IndexResults`，组装中立值后调用 `WriteOutputValue`。可选输出跳过缺失行并保留原始行号。
- Definition 与回调共用 typed 逻辑端口，实际 key 通过 `options.Port(逻辑端口)` 读取。Converter 校验结果基数、来源与业务状态，组装 JSON 等完整响应。
- Node 只发布中性结果；外部字段名、默认值和序列化格式在此生成。共享序列化函数放在本目录私有头中，例如 `rule_match_response.h`。
- binding 负责平台成员、业务枚举、字符串表示、metadata 与输出布局。按 view 中真实池规格检查容量并复制，不从字符串内容长度推断容量，不静默截断或忽略内嵌 NUL。
- 写入中途失败可能已修改租用块；Operator 保证全部输出成功后一起发布，任一失败则归还全部租约。Converter 不保留宿主指针或把请求局部指针写入输出。

平台 binding 的注册与所有权规则见[输出分配指南](../../../doc/dev_guide/operator_output_allocation.md)。

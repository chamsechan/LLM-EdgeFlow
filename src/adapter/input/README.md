# Input Converters (输入转换器)

本目录包含完整外部请求到内部 Pipeline Blackboard 端口的业务转换器。

## 规范与契约

- 通过 `REGISTER_INPUT_CONVERTER` 注册 `InputConverterDefinition`；Definition 与回调共用 typed 端口常量。
- 槽用 `ExternalInputSlot<Value>` 声明 `adapter/io_values.h` 中的中立值类型。binding 校验平台载体并复制其内容，Converter 只接收自有字符串、数组与标量。
- 单槽、每请求一个载荷使用 `DecodeRequestRows<Value>`；业务函数接收 `const Value&`，返回自持有载荷与 `AdapterStatus`。框架处理遍历、来源、端口绑定及错误位置。
- 候选展开等算法显式使用 `ValidateDecodeRequest` / `ReadInputSlot<Value>`，后者返回 `std::optional<Value>`。Converter 保留业务校验，例如 JSON 字段要求和 rerank 候选段落的 64 KiB 上限。
- 平台类型、成员访问、字符串长度、metadata 和业务枚举映射由 binding 处理。Operator 核对输入输出批次大小并按行配对；Converter 为内部载荷写入批内行号，不读取宿主请求 ID 或图像帧序号。
- 通过 `options.Port(逻辑端口)` 发布至 `AlgContext`；未引用端口返回空名并跳过发布。转换器无请求间共享状态，不保留宿主指针。
- 输入校验失败立即终止，不租用输出块、不执行 Pipeline；输出池存储已在 Create 阶段分配。

平台 binding 的注册与所有权规则见[输出分配指南](../../../doc/dev_guide/operator_output_allocation.md)。

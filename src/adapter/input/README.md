# Input Converters (输入转换器)

本目录包含所有外部输入协议到内部 Pipeline Blackboard 端口的独立输入转换器。

## 规范与契约
- 每个输入转换器通过 `REGISTER_INPUT_CONVERTER` 注册 `InputConverterDefinition`。
- Definition 与回调的 `bindings.Key(port)` 复用同一 typed 端口；普通外部槽用 `ExternalInputSlot<T>`。
- `ValidateDecodeRequest` 与 Definition 共用批次常量，`ReadInputSlot` 处理槽取得；文本可使用 `IsValidInputString` / `CopyInputString`，PCM 和候选展开继续显式处理。
- 负责外部载体批次envelope校验、类型转换、深拷贝（copy-in）以及按端口映射发布到 `AlgContext`。
- 保证无请求间共享状态与局部临时引用的生命周期隔离。
- 输入校验失败时立即终止，不分配后续输出池资源，不触发 Pipeline 执行。

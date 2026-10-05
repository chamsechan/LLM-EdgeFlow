# Input Converters (输入转换器)

本目录包含所有外部输入协议到内部 Pipeline Blackboard 端口的独立输入转换器。

## 规范与契约
- 每个输入转换器通过 `REGISTER_INPUT_CONVERTER` 注册 `InputConverterDefinition`。
- Definition 与回调的 `bindings.Key(port)` 复用同一 typed 端口；普通外部槽用 `ExternalInputSlot<T>`。
- 单槽、每请求一个载荷使用 `DecodeRequestRows`；业务函数接收宿主值、返回自持有载荷与 `AdapterStatus`，框架处理批次、来源、绑定及错误位置。文本和 PCM 是已编译示例。
- 多槽和候选展开继续显式使用 `ValidateDecodeRequest` / `ReadInputSlot`；批次上限默认采用绑定的框架标准值 64、Operator 经 `InputDecodeOptions` 传入，转换器不另设常量。文本结构检查和复制使用 `IsValidInputString` / `CopyInputString`。
- 输入长度上限引用 `adapter/biz_input_constraints.h`；只有业务确需更严格的限制时，才在转换器中定义具名常量。rerank 候选段落保留 64 KiB 的转换器限制。
- 负责外部载体批次envelope校验、类型转换、深拷贝（copy-in）以及按声明的端口发布到 `AlgContext`。
- 保证无请求间共享状态与局部临时引用的生命周期隔离。
- 输入校验失败时立即终止，不租用输出池块，不触发 Pipeline 执行；输出池存储已在 Create 阶段分配。

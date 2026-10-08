# Input Converters (输入转换器)

本目录包含所有外部输入协议到内部 Pipeline Blackboard 端口的独立输入转换器。

## 规范与契约
- 每个输入转换器通过 `REGISTER_INPUT_CONVERTER` 注册 `InputConverterDefinition`，标识一种（结构体，业务）：
  `type` 是宿主结构体（map key 后缀，如 `doc_in`），`name` 是业务名（如 `doc_qa`），
  `service_type` 是该业务在结构体 `service_type` 成员上的取值。框架在每个请求调用转换函数前核对它；
  `common`（保留的业务名，表示该结构体的默认处理）和没有该成员的结构体（`CompanyString`）不填。
  同一结构体的多个业务各自登记，解析结构体的公共代码抽成函数共用（`entity_in` 有 `entity_extract`、`translate`）；
  新增业务只新增登记，不改已有的分发代码。
- 登记的 `service_type` 取自平台模拟的占位枚举，进内网后按真实头文件核对。
- 需要调节的行为写成 `params`（`Parameters<Params>`，与 Node 相同），回调通过 `options.Params<Params>()` 取只读共享的参数；
  目前生产输入转换器没有参数。
- Definition 与回调复用同一 typed 端口常量；普通外部槽用 `ExternalInputSlot<T>`。
- 单槽、每请求一个载荷使用 `DecodeRequestRows`；业务函数接收宿主值、返回自持有载荷与 `AdapterStatus`，框架处理批次、来源、绑定及错误位置。文本和 PCM 是已编译示例。
- 多槽和候选展开继续显式使用 `ValidateDecodeRequest` / `ReadInputSlot`；批次上限是框架常量 64（再按输出池深收紧），由 Operator 在解码前检查，转换器不另设常量。文本结构检查和复制使用 `IsValidInputString` / `CopyInputString`。
- 输入长度上限引用 `adapter/biz_input_constraints.h`；只有业务确需更严格的限制时，才在转换器中定义具名常量。rerank 候选段落保留 64 KiB 的转换器限制。
- 负责外部载体批次envelope校验、类型转换、深拷贝（copy-in）以及按声明的端口发布到 `AlgContext`。
- 保证无请求间共享状态与局部临时引用的生命周期隔离。
- 输入校验失败时立即终止，不租用输出池块，不触发 Pipeline 执行；输出池存储已在 Create 阶段分配。

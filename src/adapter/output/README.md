# Output Converters (输出转换器)

本目录包含所有内部 Pipeline Blackboard 端口到外部输出协议的独立输出转换器。

## 规范与契约
- 每个输出转换器通过 `REGISTER_OUTPUT_CONVERTER` 注册 `OutputConverterDefinition`，标识一种（结构体，业务）：
  `type`、`name` 和 `service_type` 的含义与输入转换器相同；转换器写出时把输出结构体的 `service_type` 设为登记的取值，
  `common` 和没有该成员的结构体不填。
- 输出结构的每个字符串字段都有一个 `<字段>_max_bytes` 尺寸参数：用 `MaxBytes("field", &Params::field_max_bytes).Default(n)` 声明，
  默认值在 1 到平台上限之间（注册审计检查），方案在 `io.output` 项的 `params` 中覆盖。没有字符串字段的结构体（rerank）没有尺寸参数。
- 槽声明（`ExternalOutputSlot<T>(type)`）固定命名布局、布局参数和 metadata（`allocator`、`allocator_params`、
  `metadata_count`、`metadata_type_id`），不进入配置；要用另一种布局，就换一个登记。
- 其他行为参数同样写成 `params`，回调通过 `options.Params<Params>()` 读取。
- 单槽、每请求一个结果使用 `EncodeResultRows`；业务函数接收结果与借用的宿主输出，用 `OutputStringWriter` 写入字符串。框架处理绑定、结果重排、外部编号和 `written_count`。
- Definition 与 `ReadOutputValue` 共用 typed 逻辑端口，实际 key 始终通过 bindings 解析；结果来源复用已有 `IndexResults`。
- Node 只发布中性结果；外部字段名、默认值和序列化格式在此处生成。多个转换器共用同一外部格式时，序列化函数放在本目录的私有头中，例如 `rule_match_response.h`。
- 负责从 `AlgContext` 读取执行产物，校验基数与来源连续性（provenance），编码并安全写入已分配好的外部输出目标或 Operator 输出池。
- 输出容量检查失败时返回缓冲区不足并标明约束，不静默截断字符串。
- 字符串用 `WriteOutputString` 读取 view 中真实池规格（即尺寸参数生成的容量），不抄容量默认值；手工测试视图也必须提供实际容量。排名数组等非字符串输出保持专用填充。
- 字符串传递显式长度，不静默截断内嵌 NUL。中途失败可能已写入租用块，Operator 保证不发布并归还租约；writer 和目标指针不能保留到回调之外。

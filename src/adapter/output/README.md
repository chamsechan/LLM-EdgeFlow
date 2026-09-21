# Output Converters (输出转换器)

本目录包含所有内部 Pipeline Blackboard 端口到外部输出协议的独立输出转换器。

## 规范与契约
- 每个输出转换器通过 `REGISTER_OUTPUT_CONVERTER` 注册 `OutputConverterDefinition`。
- Definition 与 `ReadOutputValue` 共用 typed 逻辑端口，实际 key 始终通过 bindings 解析；结果来源复用已有 `IndexResults`。
- 负责从 `AlgContext` 读取执行产物，校验基数与来源连续性（provenance），编码并安全写入已分配好的外部输出目标或 Operator 输出池。
- 输出容量检查失败时返回缓冲区不足并标明约束，不静默截断字符串。
- 字符串用 `WriteOutputString` 读取 view 中真实池规格，不抄容量默认值；手工测试视图也必须提供实际容量。排名数组等非字符串输出保持专用填充。

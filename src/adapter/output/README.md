# Output Converters (输出转换器)

本目录包含所有内部 Pipeline Blackboard 端口到外部输出协议的独立输出转换器。

## 规范与契约
- 每个输出转换器通过 `REGISTER_OUTPUT_CONVERTER` 注册 `OutputConverterDefinition`。
- 负责从 `AlgContext` 读取执行产物，校验基数与来源连续性（provenance），编码并安全写入已分配好的外部输出目标或 Operator 输出池。
- 输出容量检查失败时返回缓冲区不足并标明约束，不静默截断字符串。

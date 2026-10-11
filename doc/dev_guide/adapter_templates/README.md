# Adapter 边界示例

生产载体读取参考 [平台 mock binding](../../../src/adapter/operator/mock/platform_value_binding.cpp)，
字段选择和业务组装分别参考 [InputConverter](../../../src/adapter/input/) 与
[OutputConverter](../../../src/adapter/output/)。平台 mock 结构只位于 `include/platform_mock/`；
中立值不承诺不同业务具有相同载荷语义。

[平台载体契约测试](../../../tests/contract/abi/test_adapter_contract_security.cpp) 直接执行实际
binding，验证字符串、嵌套字段、带行 padding 的 RGB、PCM 和候选数组的复制及边界。
[Converter 公共机制测试](../../../tests/unit/adapter/test_converter_contracts.cpp) 负责行关联和
原子发布；[业务 Converter 测试](../../../tests/unit/adapter/test_io_converters.cpp) 保留完整字段预期。
这些是生产行为证据，不再维护只验证独立假 DTO 的平行示例实现。

新增业务从 [业务接入说明](../business_onboarding.md) 开始，用目标构建的
`alg_pipeline_tool catalog` 查询实际注册的业务和端口。

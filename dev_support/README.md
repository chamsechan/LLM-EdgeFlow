# 开发支持

`inference/test_biz_models.cpp` 为 mock Demo Profile 和测试 CLI 提供最小确定性能力。
五个 Model 持有中性 fixture Session；Backend 只声明策略和并发，不伪装厂商推理协议。
一次 OBJECT 链接包含全部注册，SDK 和生产 CLI 不链接这些替身。

`node_authoring/starter_*.cpp` 是当前 Node 作者教程和脚手架的普通 C++ 模板。LLM、Control
生成副本和其最小测试由既有 runner 编译执行；其余示例展示多模型及批次 API，按原样编译。
测试自己的算法预期仍留在对应业务用例，通用规则由公共机制测试负责。

没有使用方的历史 helper、对照基线和重复业务测试不保留。测试维护和执行见
[tests/README.md](../tests/README.md)。

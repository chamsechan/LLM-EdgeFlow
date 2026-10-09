---
name: edgeflow-adapter-developer
description: 新增或修改 LLM-EdgeFlow Adapter 的业务输入输出、InputConverter、OutputConverter 登记、converter 参数和输出尺寸。用于完整 Operator 请求/响应接入，不用于 Node 算法或单纯 Pipeline 配置。
---

# Adapter 业务接入开发

遵循 [CONTRIBUTING](../../../CONTRIBUTING.md)；读取共享的
[Integration 契约](../llm-edgeflow-developer-guide/references/integration.md) 和
[业务接入步骤](../../../doc/dev_guide/business_onboarding.md) 中本次涉及的部分。
外部公共接口保持用户约定；新增内部注册不意味着需要改变 Operator ABI。

## 确定新增范围

对照目标 Catalog 的 `input_converters`、`output_converters`（每项的 `type`、`name`、`service_type`、端口、参数）和相关源码，
核对完整请求/响应字段及语义。外部契约不变时复用现有登记，只在方案 `io` 中选择并按需覆盖参数。
契约改变时只新增或修改缺失的一侧；先明确载体、所有权、容量、批次和失败行为。

## 实现和登记

| 需要的部件 | 位置与操作 |
| --- | --- |
| 请求校验、字段选择与内部 payload | `src/adapter/input/`，`InputConverterDefinition`（`type` 宿主结构体、`name` 业务、`service_type`、槽声明、端口）+ `REGISTER_INPUT_CONVERTER` |
| 完整响应组装、序列化和拷贝 | `src/adapter/output/`，`OutputConverterDefinition` + `REGISTER_OUTPUT_CONVERTER`；每个字符串字段一个 `MaxBytes` 尺寸参数（`<字段>_max_bytes`），命名布局与 metadata 写在槽声明里 |
| 新业务或同一结构体的新格式 | 不另设业务声明：新增（结构体，业务）登记即可，方案 `io` 以（`type`, `name`）选择；批次上限是框架常量 64，再按池深收紧；转换器端口名即 Blackboard Key，不做改名；`common` 是保留的默认处理，其登记由用户补充 |
| 确需新的宿主值类型/分配方式 | `include/adapter/operator_value_type.h`，按 [输出分配指南](../../../doc/dev_guide/operator_output_allocation.md) 注册 |

从 [翻译输入](../../../src/adapter/input/translate_json_input.cpp)、
[翻译输出](../../../src/adapter/output/translation_json_output.cpp)（含 `MaxBytes` 尺寸参数写法）选择相近模板。
单槽、单内部业务 payload 流、每请求一个结果的路径使用（输入槽必需，输出槽可选）
`DecodeRequestRows` / `EncodeResultRows`：业务函数只处理一行载荷，框架负责遍历、来源和诊断。
若还有独立透传 metadata 流（例如只在最终响应保留的业务 ID），即使只有一个外部槽也应
使用显式多流转换和现有低层 helper；不要将 metadata 拼入模型输入。多槽或聚合同样按
实际算法处理，不强塞行模板。
字符串通过 `OutputStringWriter` 按实际容量和长度写入，不保存借用指针。

请求字段解析和最终协议组装留在 Converter；不向 Operator 中央分发添加业务分支，
也不把这些操作放进 Demo、Node 或 Core。每个（结构体，业务）只登记一个转换器，`service_type` 由框架核对；外部格式不同就是另一个
业务名（或另一个结构体）的登记。批次限制及池深收紧由共享 Integration 规则处理。
若需 Demo，只补载体、调用与展示注册，不复制业务转换。

## 验证新增路径

复用 `tests/unit/adapter/` 与 `tests/contract/abi/` 的相关套件，使用
`tests/support/adapter_harness.h`、`operator_test_fixture.h`。至少覆盖正常完整对象、
契约要求的无效输入、输出容量不足、批次限额、逐条 ID/状态及失败无残留输出。
借用内存和多槽修改再补生命周期/释放断言。

```bash
cmake --build build --target edgeflow_test_adapter_runner alg_pipeline_tool -j 4
./build/edgeflow_test_adapter_runner --gtest_list_tests
./build/alg_pipeline_tool catalog --io-binding <业务名>
```

按改动运行列出的实际测试过滤器；业务契约测试须直接调用 Operator `Process`。
用同一构建 validate/plan 目标 Pipeline 并运行实际配置。最终证据和门禁见
[Verification](../llm-edgeflow-developer-guide/references/verification.md)。

---
name: edgeflow-adapter-developer
description: 新增或修改 LLM-EdgeFlow Adapter 的业务输入输出、InputConverter、OutputConverter、I/O 参数和输出容量。用于完整 Operator 请求/响应接入，不用于 Node 算法或单纯 Pipeline 配置。
---

# Adapter 业务接入开发

遵循 [CONTRIBUTING](../../../CONTRIBUTING.md)；读取共享的
[Integration 契约](../llm-edgeflow-developer-guide/references/integration.md) 和
[业务接入步骤](../../../doc/dev_guide/business_onboarding.md) 中本次涉及的部分。
外部公共接口保持用户约定；新增内部注册不意味着需要改变 Operator ABI。

## 确定新增范围

对照目标 Catalog 的 `input_converters`、`output_converters` 和相关源码，
核对完整请求/响应字段及语义。外部契约不变时复用现有转换器，只新增需要的配置。
契约改变时只新增或修改缺失的一侧；先明确载体、所有权、容量、批次和失败行为。

## 实现和登记

| 需要的部件 | 位置与操作 |
| --- | --- |
| 请求校验、字段选择与内部 payload | `src/adapter/input/`，`InputConverterDefinition` + `REGISTER_INPUT_CONVERTER` |
| 完整响应组装、序列化和拷贝 | `src/adapter/output/`，`OutputConverterDefinition` + `REGISTER_OUTPUT_CONVERTER` |
| 接入选择与边界 | Pipeline 根 `io.input` / `io.output` 以 `(type, name)` 选择登记；每个登记拥有一个宿主槽，转换器 typed 逻辑端口组合成 Core 的明确边界，输出项 `inputs` 指定每个必填端口的来源 |
| 确需新的宿主值类型/分配方式 | `include/adapter/operator_value_type.h`，按 [输出分配指南](../../../doc/dev_guide/operator_output_allocation.md) 注册 |

从 [翻译输入](../../../src/adapter/input/translate_json_input.cpp)、
[翻译输出](../../../src/adapter/output/translation_json_output.cpp) 选择相近模板。
单内部业务 payload 流、每请求一个结果的路径使用
`DecodeRequestRows` / `EncodeResultRows`：业务函数只处理一行载荷，框架负责遍历、来源和诊断。
若还有独立透传 metadata 流（例如只在最终响应保留的业务 ID），即使只有一个外部槽也应
使用显式多流转换和现有低层 helper；不要将 metadata 拼入模型输入。多槽或聚合同样按
实际算法处理，不强塞行模板。
Converter 只处理 `adapter/io_values.h` 中的请求自有值，不包含平台类型或读取平台成员。
槽用 `ExternalInputSlot<Value>` / `ExternalOutputSlot<Value>` 声明中立类型；对应 binding
通过 `SetInputValue<Host, Value>` / `SetOutputValue<Host, Value>` 提供读取和写入。
输入行函数接收 `const Value&`；输出行函数接收 `Value*`，直接赋值 `std::string`。
多流输出组装中立值后调用 `WriteOutputValue`，由 binding 按真实池容量写入。
平台布局、字符串表示、metadata 和业务枚举映射只在 binding 中处理；
当前模拟平台的 traits、字符串和分配 helper 位于 `adapter/platform_value_binding.h`。

请求字段解析和最终协议组装留在 Converter；不向 Operator 中央分发添加业务分支，
也不把这些操作放进 Demo、Node 或 Core。`type` 是宿主后缀，`name` 对应业务值；
`common` 表示该载体的默认处理。binding 的 `services` 维护业务名到平台枚举的映射，
Converter 不声明平台枚举。Operator 输入输出 vector 按同一行关联，载体无需请求 ID；
内部 `req_id` 为批内行号，输出视图 `count` 为原始批大小，包含可选输出缺省行。参数通过 `Parameters<P>` 声明；字符串尺寸使用
`MaxBytes`，默认值在转换器中声明，平台登记只保留硬上限。`Prepare` / `Validate` 在创建时执行，
运行时通过 `options.Params<P>()` 只读访问。端口以 `options.Port(逻辑名)` 读写；
未引用的输入端口返回空名，由发布 helper 跳过。限额共享头为 `adapter/input_limits.h`。有效批次上限为 `min(max_frame_depth, 64)`。
若需 Demo，只补载体、调用与展示注册，不复制业务转换。

## 验证新增路径

复用 `tests/unit/adapter/` 与 `tests/contract/abi/` 的相关套件，使用
`tests/support/adapter_harness.h`、`operator_test_fixture.h`。至少覆盖正常完整对象、
契约要求的无效输入、输出容量不足、批次限额、逐行对应关系/状态及失败无残留输出。
借用内存和多槽修改再补生命周期/释放断言。

```bash
cmake --build build --target edgeflow_test_adapter_runner alg_pipeline_tool -j 4
./build/edgeflow_test_adapter_runner --gtest_list_tests
./build/alg_pipeline_tool catalog
```

按改动运行列出的实际测试过滤器；业务契约测试须直接调用 Operator `Process`。
用同一构建 validate/plan 目标 Pipeline 并运行实际配置。最终证据和门禁见
[Verification](../llm-edgeflow-developer-guide/references/verification.md)。

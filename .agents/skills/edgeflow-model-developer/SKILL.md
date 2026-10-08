---
name: edgeflow-model-developer
description: 新增或修改 LLM-EdgeFlow Model 的预处理、输出解释和能力语义，复用中性 Backend 协议并注册 ModelDefinition。更换兼容权重或参数使用配置 skill，新增厂商运行时使用 Backend skill。
---

# Model 语义实现

先用目标工具 `describe-model` / `describe-backend` 判断现有语义和协议是否可复用。
只换兼容权重不新增 Model。读取
[Model Execution 契约](../llm-edgeflow-developer-guide/references/model-execution.md)；
设计与交付遵循 [CONTRIBUTING](../../../CONTRIBUTING.md)。

## 实现与注册

- 在 `src/engine/models/<operation_or_model>/` 实现
  [model_interface.h](../../../include/engine/model_interface.h) 的强类型能力接口；
  创建入口接收 `ModelCreateContext`，通过中性 Backend Session 执行。
- Model 负责 tokenizer、prompt 格式、图像/文本预处理和输出解释；厂商头文件及资源句柄
  不进入 Model。需要新执行运行时，转 [Backend skill](../edgeflow-backend-developer/SKILL.md)；
  必须扩展中性协议时按跨层契约变更处理，不能直接引用具体 Backend。
- 通过 `REGISTER_MODEL_WITH_DEFINITION` 注册完整 `ModelDefinition`，声明能力、执行协议、
  并发约束及参数 `params`。身份只写一次：继承 `ModelIdentity<Model, 能力接口>` 并声明
  `kModelType`、`kConcurrency`，Definition 从 `MakeModelDefinition<Model>()` 开始。
  不要在 Web、skill 或 Node 再维护模型列表。
- 参数与 Node、converter 同一种四段写法，改参数只动前两段：① `Params` 结构体；
  ② `Parameters<Params>` 声明，`Field(...)` 一次写明名字、默认值、范围、枚举和中文说明，
  跨字段规则写在 `.Validate(...)`（纯函数，不做文件/资源 I/O）；③ 登记
  `def.params = ParamSpec();`；④ 取用 `ctx.Params<Params>()`。默认值只写在声明里，
  结构体成员不重复。
- 参数只在 `ParameterSet::Parse` 校验一次，Validator 预检和 ModelRuntimeFactory 共用，
  Backend 创建/加载前就会拒绝非法参数。`Create` 不再复查范围、枚举、必填或跨字段规则，
  只做依赖已加载内容的检查：会话接口类型、批策略、Tensor metadata、文件能否打开。
  工厂已核对会话协议，不要在 `Create` 里再比较一次。
- 可以不写的参数用 `std::optional<T>` 成员（不能同时 `Required()` / `Default()`）。
  能从模型文件读到的事实（如 ONNX 固定形状）用
  [`ResolveFromModel`](../../../src/engine/models/common/from_model.h) 取最终值：
  配置写了就与模型核对，没写就用模型的值，读不到再用后备值或要求填写。
- 固定 Tensor 批处理用 `FixedBatchExecutor::Execute`；普通逐项路径用 `ExecuteItems`。
  遵循各 helper 的执行/补齐协议，框架负责 provenance、去 padding 和失败清空；
  不把非空固定批改成逐项调用。BatchPolicy 来自 Session。
- 源码放在 `src/engine/models/<model>/` 下，下次构建时自动编入 `edgeflow_model_execution_objects`。

只选相近模板：[BGE embedding](../../../src/engine/models/bge_embedding/bge_embedding_model.cpp)
展示 Tensor 语义、固定批与 `ResolveFromModel`；[Qwen](../../../src/engine/models/qwen_causal_lm/qwen_causal_lm_model.cpp)
展示文本生成；[Vision](../../../src/engine/models/vision_document/vision_document_model.cpp)
展示参数的 `Validate` 规则与逐项执行。

## 验证责任

在 `tests/unit/engine/` 用确定性 Session 测试预处理/输出语义、能力和协议不匹配、批次边界、
padding 与来源、失败清空。参数规则在参数层断言：用
[`tests/support/parameter_support.h`](../../../tests/support/parameter_support.h) 的
`ParseModelParams` 按已注册 Definition 解析配置，非法参数应在 Backend 创建/Load 前拒绝；
把每个实现的默认值、覆盖和非法值加入
`tests/unit/engine/test_model_backend_parameters.cpp`。
`ModelConfigValidationTest` fixture 由 `ModelBackendDecouplingTest` 的 CTest 入口覆盖。

```bash
cmake --build build --target edgeflow_test_core_runner alg_pipeline_tool -j 4
(cd build && ctest -R '^(ModelBackendDecouplingTest|BatchExecutorTest)$' --output-on-failure)
./build/alg_pipeline_tool describe-model <model_type>
```

再运行本模型的实际测试过滤器，检查注册后的方案 validate/plan 和加载路径。
真实模型效果需要相应权重与样例，不以 Session 替身推断。
最终门禁与证据见 [Verification](../llm-edgeflow-developer-guide/references/verification.md)。

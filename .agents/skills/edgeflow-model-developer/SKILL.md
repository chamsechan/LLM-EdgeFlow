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
  并发约束及 `params`。身份只写一次：继承 `ModelIdentity<Model, 能力接口>` 并声明
  `kImplName`、`kConcurrency`，Definition 从 `MakeModelDefinition<Model>()` 开始。
  不要在 Web、skill 或 Node 再维护模型列表。
- 参数按四段编写：`Params` 结构、返回 `Parameters<Params>` 的 `ParamSpec()`、
  `def.params = ParamSpec()`、创建时 `ctx.Params<Params>()`。默认值仅写在 `Field` 声明中；
  纯参数语义用 `ParamSpec().Validate`，不做文件/资源 I/O。
  Validator 在规划时执行参数校验，Factory 在创建前分别解析模型、后端参数一次，
  `Create` 读取只读参数，不复查纯参数。直接调用 `Create` 的测试须先用 Definition 的
  `params.Parse` 产生参数。资源存在性、Tensor metadata 和会话检查留在创建阶段。
- `std::optional<T>` 字段不写时为空，不能声明 `Required()` 或 `Default()`。
  BGE 用 `ResolveFromModel` 读取固定形状；配置与固定值冲突时报错，无法读取时用已声明
  fallback 或提示填写参数。Catalog 展示声明，创建日志记录最终值。
- Pipeline 模型条目使用 `{type, name, file, params?, backend: {type, params?}}`。
  `type` 是接口对应的类别，`impl_name` 是 C++ 实现名；按类别与后端协议唯一选择实现，
  注册审计和 Core 都拒绝同一组合的歧义。测试模型用 `kFixture` 并声明 `fixture_backends`。
- 文件参数用 `.File()`，相对 Pipeline JSON 目录的检查和解析属于接入层；模型直接使用
  已解析路径。批大小属于 Backend。`normalize`、池化和词缀属于向量模型；
  `system_prompt`、`random_seed`、转写 `language` 属于节点调用选项。
- 固定 Tensor 批处理用 `FixedBatchExecutor::Execute`；普通逐项路径用 `ExecuteItems`。
  遵循各 helper 的执行/补齐协议，框架负责 provenance、去 padding 和失败清空；
  不把非空固定批改成逐项调用。BatchPolicy 来自 Session。
- 源码放在 `src/engine/models/<model>/` 下，下次构建时自动编入 `edgeflow_model_execution_objects`。

只选相近模板：[BGE embedding](../../../src/engine/models/bge_embedding/bge_embedding_model.cpp)
展示 Tensor 语义与固定批；[Qwen](../../../src/engine/models/qwen_causal_lm/qwen_causal_lm_model.cpp)
展示文本生成；[Vision](../../../src/engine/models/vision_document/vision_document_model.cpp)
展示纯配置预检与逐项执行。

## 验证责任

在 `tests/unit/engine/` 用确定性 Session 测试预处理/输出语义、能力和协议不匹配、批次边界、
padding 与来源、失败清空；无效语义配置应在 Backend 创建/Load 前拒绝。
`ModelConfigValidationTest` fixture 由 `ModelBackendDecouplingTest` 的 CTest 入口覆盖。

```bash
cmake --build build --target edgeflow_test_core_runner alg_pipeline_tool -j 4
(cd build && ctest -R '^(ModelBackendDecouplingTest|BatchExecutorTest)$' --output-on-failure)
./build/alg_pipeline_tool describe-model <impl_name>
```

再运行本模型的实际测试过滤器，检查注册后的方案 validate/plan 和加载路径。
真实模型效果需要相应权重与样例，不以 Session 替身推断。
最终门禁与证据见 [Verification](../llm-edgeflow-developer-guide/references/verification.md)。

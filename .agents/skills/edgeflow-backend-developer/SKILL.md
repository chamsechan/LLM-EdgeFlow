---
name: edgeflow-backend-developer
description: 新增或修改 LLM-EdgeFlow Backend 的厂商 SDK、硬件运行时、资源加载或中性 Session 实现，并处理 CMake 依赖隔离与验证。模型前后处理属于 Model，不因新增业务或权重而新增 Backend。
---

# Backend 运行时接入

先确认现有 Backend 无法提供所需运行时；读取
[Model Execution 契约](../llm-edgeflow-developer-guide/references/model-execution.md)。
新增依赖/协议、所有权和并发设计按 [CONTRIBUTING](../../../CONTRIBUTING.md) 记录。
公司内部 SDK 的可访问范围遵循 [仓库边界](../../../AGENTS.md#repository-guardrails)，
外部环境只实现有依据的中性接口与明确的 mock，不猜测真实厂商布局或枚举。

## Session 与资源

1. 在 `src/engine/backends/<backend>/` 实现
   [IInferenceBackend](../../../include/engine/backend_interface.h) 的 `Load`，
   返回对应中性协议的 Session；参考
   [ONNX](../../../src/engine/backends/onnxruntime/onnxruntime_backend.cpp) 或
   [llama.cpp](../../../src/engine/backends/llama_cpp/llama_cpp_backend.cpp)。
2. 明确资源创建/失败回收/销毁顺序、输入输出内存生命周期、执行协议、真实并发能力和
   BatchPolicy。`BackendLoadSpec::requested_protocol` 必须匹配，尽早拒绝不支持的协议。
   Session 实际协议仍需与声明一致。
3. Provider 继承 `BackendIdentity<Backend>` 并只在 `kBackendType` 声明类型名；从
   `MakeBackendDefinition<Backend>()` 开始注册 `BackendDefinition` 与
   `REGISTER_BACKEND_WITH_DEFINITION`，声明协议、并发和参数。参数与 Model、Node 同一种四段写法：
   `Params` 结构体、`Parameters<Params>` 声明（`Field(...)` 写明默认值、范围、枚举和中文说明，
   跨字段规则写在 `.Validate(...)`，如 llama.cpp 的 `decode_batch_size` 不大于
   `context_size`）、`def.params = ParamSpec();`，`Load` 里用 `spec.Params<Params>()` 取用。
   参数已由 `ParameterSet::Parse` 校验并补齐默认值，`Load` 不再解析配置或检查未知字段，
   只处理依赖环境的检查（路径、设备、平台）。
   prompt 格式、图像解码、向量池化等模型语义交给 Model。

## 把 vendor 依赖留在 Backend

- 源码放在 `src/engine/backends/<backend>/` 下，下次构建时自动编入
  **`edgeflow_model_execution_backends_objects`**。Models/Runtime 属于另一个 OBJECT，
  不共享 vendor 编译要求。
- 依赖获取、版本 pin 和校验放 `cmake_ext/`；厂商头、`HAVE_*`、imported target 编译需求
  只以 `PRIVATE` 提供给 Backend OBJECT。最终 SDK/内部运行时通过 `LINK_ONLY` 保留
  必要传递链接，不把 vendor include 或宏扩散到上层。
- 新增 vendor 时同步 `scripts/check_layer_dependencies.py` 的所属 Backend 规则、
  `cmake_ext/LayerHeaderViews.cmake` 与相应编译探针；沿用现有构建开关和启用/关闭注册规则。
  测试确需宏时仅向相关测试 target 显式 PRIVATE 提供。

## 验证可执行边界

沿用 `tests/unit/engine/` 和 `edgeflow_test_core_runner`，覆盖非法参数（在参数层用
`tests/support/parameter_support.h` 的 `ParseBackendParams` 断言）、非法路径、协议错误、
资源创建失败与释放、声明支持的 batch/并发行为，以及关闭 Backend 时的行为。
按实际改动选择 `ModelBackendDecouplingTest`、具体 Backend 套件和 `LayerGuardTest`。
新测试加入现有 inventory/runner，不为一个 Backend 另建框架。

实施后，在具备实际依赖的授权环境，至少对所改 Backend 启用与关闭配置分别构建
`alg_sdk` 和 `alg_pipeline_tool`，检查 Catalog
注册及上下层编译隔离，并运行相关测试；同一构建目录顺序使用，不能并发重配置。
非默认构建的工具、Catalog 和测试必须来自同一目录，不执行旧配置留下的测试二进制。
用 `describe-backend` 和实际 Model 组合 validate/plan；加载/推理验收另需可用资产。
依赖缺失时仅报告已验证的中性接口或隔离 mock，并列出真实 Backend 构建/执行未验证项。
用户只请求接入建议时交付上述设计与验证计划，不实施 vendor 接入。

最终默认门禁、跳过项和真实硬件限制见
[Verification](../llm-edgeflow-developer-guide/references/verification.md)。

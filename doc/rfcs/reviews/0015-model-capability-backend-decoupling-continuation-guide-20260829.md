# RFC-0015 后续实现指南

- **制定日期**：2026-08-29
- **适用分支**：`feat/model-backend-decoupling-rfc`
- **关联 RFC**：[RFC-0015 模型能力与推理运行时解耦](../0015-model-capability-backend-decoupling.md)
- **整改依据**：[整改计划](0015-model-capability-backend-decoupling-remediation-plan-20260829.md)
- **当前状态**：Neutral Contract/Registry 基线已建立，完整 Pipeline 与真实 Backend 尚未实现

## 1. 当前基线

当前提交已经提供以下可继续开发的基础：

1. 中性 inference payload、`TraceableItem<T>`、Config schema、Tensor、Model/Backend
   Definition；
2. `IModel`、各 capability 接口、`IInferenceBackend`、Tensor Graph/Causal LM
   session 接口；
3. fail-closed 的 `ModelRegistry`、`BackendRegistry` 和按值 Catalog snapshot；
4. `ModelRuntimeFactory` 基础物化、静态 protocol 检查、session identity/concurrency/
   BatchPolicy 检查；
5. `ModelManager::RegisterBatch` staging + 原子提交框架；
6. 新版 `FixedBatchExecutor::Execute` 的严格数量、provenance 和失败回滚；
7. 仅存在于 `tests/support/inference/` 的 fake backend；
8. 生产 Catalog 中 `models=[]`、`backends=[]`，没有虚假的生产能力；
9. 全量 CTest 和 `./scripts/run_all_tests.sh --full` 已通过。

以下内容仍是有意保留的未完成状态：

- Parser、Validator、Pipeline Build 仍以 `engine_type + EngineFactory` 为主路径；
- `ValidatedModelPlan` 已有类型定义，但尚未由 Validator 完整填充和消费；
- Node 仍主要绑定旧 `IModelEngine` capability；
- 没有生产 `OnnxRuntimeBackend`、`LlamaCppBackend` 或具体 Model；
- 旧组合 Engine、Mock Engine 和旧配置尚未删除；
- RFC 必须继续保持 `In Implementation`，不得提前标记 `Completed`。

## 2. 开发期间必须遵守的规则

1. 每个阶段结束时必须可编译、可测试，不提交固定返回成功的生产桩。
2. 测试替身只能放在 `tests/support/`，不得出现在生产 Catalog。
3. Core 只理解 Model/Backend Definition、schema 和中性协议，不读取 vendor 私有字段。
4. Backend 只负责 vendor runtime、模型文件加载和中性执行协议，不实现 tokenizer、pooling、
   normalize、sampling 或业务语义。
5. Model 只负责 capability 语义和预后处理，不直接包含 ONNX Runtime、llama.cpp 等 vendor
   类型。
6. Node 只做 Blackboard I/O、选项传递和错误映射，不复制 Model 推理逻辑。
7. 所有固定批推理都调用唯一 `FixedBatchExecutor::Execute`。
8. Validator 必须在任何 Backend 创建、模型加载或 Node 初始化副作用之前失败。
9. Pipeline 只能消费 `ValidatedPipelinePlan`，不得重新解析 JSON 或重复归一化 schema。
10. 每个 `noexcept` 边界必须 catch-all 并 fail-closed；失败时清空输出，不暴露部分结果。

## 3. 第一项工作：明确过渡期配置规则

现有仓库的所有业务配置仍使用旧 `engine_type/config`。如果在真实 Model/Backend 完成前直接
删除旧字段，全量 Demo 和回归会立即失效。因此，先更新 RFC 的配置迁移章节，明确阶段 2–6
使用以下临时双轨规则：

### 3.1 推荐的临时 dialect

在 `ParsedModelConfig` 中增加显式 dialect，例如：

```cpp
enum class ModelConfigDialect {
  kLegacyEngine,
  kModelBackend,
};
```

解析规则：

- 出现 `engine_type` 或旧 `config` 时，只允许完整 legacy 字段集合；
- 出现 `capability/model_type/backend/model_config/backend_config` 时，只允许完整新字段集合；
- 新旧字段混用必须返回 `kInvalidCombination`；
- 新模式必须拒绝未知字段；
- 阶段 7 删除 dialect、旧字段和 legacy 分支。

必须先把这条临时兼容规则写入 RFC，再实现代码，避免实现与 RFC 冲突。

## 4. 阶段二：完成 Pipeline 规划与原子物化

阶段二是下一步最高优先级。不要先写真实 ONNX/llama.cpp Backend。

### 4.1 Parser

修改：

- `include/core/pipeline_config.h`
- `src/core/pipeline_config.cpp`
- `tests/test_pipeline_config.cpp`

新模式只接受：

```json
{
  "model_id": "embed_model_v1",
  "capability": "embedding",
  "model_type": "bge_embedding",
  "backend": "test_tensor_backend",
  "model_path": "./models/bge/model.onnx",
  "model_config": {},
  "backend_config": {}
}
```

实现要求：

1. 五个 string 字段必填、非空；
2. `model_config`、`backend_config` 缺省归一化为 `{}`，存在时必须为 object；
3. `model_id` 在同一 Pipeline 内唯一；
4. 拒绝未知字段和新旧 dialect 混用；
5. Parser 不查询 Registry、不解析私有 config、不访问文件；
6. 每个错误提供准确 `/models/<index>/<field>` JSON Pointer。

至少覆盖 missing、empty、wrong type、unknown field、mixed dialect、duplicate ID。

### 4.2 共享 config value 校验与归一化

Definition 自身合法性已经由 `ValidateConfigFieldDefinitions` 检查。下一步需要增加一套
“按 Definition 校验用户 config 并补默认值”的单一实现，供 Validator 同时处理 Model 和
Backend config。

建议接口：

```cpp
bool ValidateAndNormalizeConfig(
    const std::vector<ConfigFieldDefinition>& schema,
    const nlohmann::json& input,
    nlohmann::json* normalized,
    std::vector<ValidationDiagnostic>* diagnostics,
    const std::string& base_pointer);
```

要求：

- 输出必须是新 object，不原地修改输入；
- 拒绝未知字段；
- 检查 required、kind、minimum、maximum、enum；
- 只从 Definition 注入默认值；
- Model 与 Backend 使用同一个实现；
- 不允许 Pipeline、RuntimeFactory 再次合并或覆盖结果。

### 4.3 Validator 和 `ValidatedModelPlan`

修改：

- `include/core/pipeline_validator.h`
- `src/core/pipeline_validator.cpp`
- `tests/test_validated_pipeline_plan.cpp`
- 新增 `tests/test_model_backend_pipeline.cpp`（推荐）

对新 dialect 严格按以下顺序执行：

1. 检查 ModelRegistry/BackendRegistry conflict；
2. 查找 `ModelDefinition`；
3. 查找 `BackendDefinition`；
4. 校验配置 capability 等于 ModelDefinition capability；
5. 校验 required protocol 在 Backend supported protocols 中；
6. 校验并归一化 `model_config`；
7. 校验并归一化 `backend_config`；
8. 在 model root 下安全解析 `model_path`；
9. 校验 Node `bind_model` 引用；
10. 校验 NodeDefinition capability；
11. 计算 effective concurrency：任一方为 serialized，则结果为 serialized；
12. 填充 `ValidatedPipelinePlan.models`。

Validator 不得调用 `BackendRegistry::Create`、`Backend::Load` 或
`ModelRuntimeFactory::Create`。使用带计数器的 fake 验证所有校验失败时 load count 为 0。

### 4.4 Pipeline 原子物化

修改：

- `src/core/pipeline.cpp`
- `include/core/session_context.h`
- `tests/test_model_backend_pipeline.cpp`

新 dialect 的唯一正确流程：

```text
ValidatedPipelinePlan.models
  → 逐项映射 ModelLoadSpec
  → 在局部 vector 创建全部 IModel
  → 构造全部 ModelRegistration
  → 一次 RegisterBatch
  → 成功后才继续 Node 初始化
```

具体要求：

- 只遍历 `plan_.models`，不读取原始 JSON；
- `ModelLoadSpec.backend_config` 必须与 plan 中 normalized value 逐值一致；
- 不注入 `RuntimeOptions.device_id/platform/chip_type`；
- revision 输入必须包含 model type、backend、resolved path、双 normalized config；
- 第二个 Model 创建失败时，第一个只存在于局部 staging，Session 中不得出现；
- `RegisterBatch` 失败时 Pipeline 进入 failed 状态；
- 所有失败对象只析构一次。

legacy dialect 暂时保持现有 Build 路径，阶段 7 删除。

### 4.5 阶段二完成条件

- test Model + test Backend 可以完成一条真实 `Pipeline::Build`；
- 新配置可 validate、plan、build；
- protocol/capability/schema 错误全部在 Backend Load 前失败；
- 两个模型中第二个失败时，Session 仍为空；
- 所有现有 legacy Pipeline 回归保持通过；
- 生产 Catalog 仍不得出现未实现 Model/Backend。

## 5. 阶段三：真实 ONNX Runtime + Embedding

阶段二全绿后再创建：

```text
src/engine/backends/onnxruntime/
src/engine/models/bge_embedding/
```

### 5.1 `OnnxRuntimeBackend`

- vendor 头文件和 `Ort::Env/Session/MemoryInfo` 只出现在 backend 目录；
- `Load` 校验模型路径、创建真实 Session、读取真实 I/O metadata；
- `ITensorGraphSession::Run` 校验 input name、dtype、rank、shape、batch、buffer byte size；
- 使用中性 `TensorMap` 作为唯一上层接口；
- 捕获全部 `Ort::Exception` 和标准异常并写 diagnostic；
- 未启用 ONNX Runtime 时不注册 `onnxruntime`，禁止 fallback 成功；
- CMake 只把 ONNX Runtime 链接到对应 backend target。

### 5.2 `BgeEmbeddingModel`

- ModelDefinition 声明 tokenizer sidecar、max length、pooling、output name 等字段；
- 实现 tokenize、truncate、padding、attention mask；
- 通过 `FixedBatchExecutor` 构造批次；
- 只调用 `ITensorGraphSession::Run`，不引用 ONNX Runtime；
- 实现 pooling、normalize、维度检查和 provenance；
- 任一批失败必须清空全部输出。

### 5.3 `TextEmbeddingNode`

- 改为 `ModelBoundNode<IEmbeddingModel>`；
- Node 只构造 `EmbeddingOptions` 并调用 `model()->Embed`；
- 删除 Node 内 normalize/固定 batch/Engine 专用逻辑；
- Blackboard Key、端口和 DAG 契约保持不变。

阶段三完成后，生产 Catalog 才允许出现真实 `onnxruntime` 和 `bge_embedding`。

## 6. 阶段四：Reranker 复用 ONNX Backend

详细的文件级步骤、固定契约、测试矩阵和完成门禁见
[阶段 4：Rerank 复用 ONNX Backend 可执行实施指南](0015-stage4-rerank-implementation-guide-20260829.md)。

1. 新增 `BgeRerankerModel`，复用同一个 `ITensorGraphSession` 协议；
2. 实现 query/candidate tokenize、pair packing、score 提取；
3. `TextRerankNode` 改绑 `IRerankModel`；
4. 保持 top-k 在 Node，模型只输出逐候选 score；
5. 删除独立 `OnnxRerankEngine` 运行时代码；
6. 测试确认 Embedding/Rerank 使用同一个 Backend Creator，没有第二份 ORT adapter。

## 7. 阶段五：llama.cpp + Qwen Causal LM

### Backend

- vendor model/context/KV cache 生命周期只在 `LlamaCppBackend` 和 session；
- session 实现 `ITokenCodec`、`CreateSequence`、`Evaluate`；
- 每个请求拥有独立 sequence state；
- Backend 不实现 chat template、sampling、stop string 或业务 prompt。

### Model

- `QwenCausalLmModel` 负责 chat template、generation loop、sampling 和 stop；
- 每次 Generate 创建独立 sequence；
- 校验 context/token 上限；
- `LlmGenerateNode` 只传 `GenerateOptions` 并调用 `ILlmModel::Generate`。

测试必须覆盖 BOS/EOS、stop、temperature/top-p、最大 token、异常和并发 sequence 隔离。

## 8. 阶段六：OCR、ASR 与测试替身

1. 建立 PPOCR/Paraformer Model capability，复用 `contracts/inference_payloads.h` DTO；
2. OCR/ASR Node 改绑 `IOcrModel`/`IAsrModel`；
3. 避免图片、音频和文档结果的无意义深拷贝；
4. 所有固定响应 fixture 移入 `tests/support/` 或 Demo 测试目标；
5. 删除生产 Mock Engine 注册；
6. 生产 Catalog 不得出现 mock、emulator、fake、stub 类型。

## 9. 阶段七：删除兼容层并收口

只有所有生产 Pipeline 已迁移后才能执行：

- 删除 `IModelEngine`、`EngineFactory`、`EngineDefinition`；
- 删除 `ParsedModelConfig.engine_type/config` 和临时 dialect；
- 删除 `ModelBoundNode::engine()` 兼容别名；
- 删除旧 ONNX/llama.cpp/Mock 组合 Engine；
- 所有配置迁移为新 models schema；
- Catalog/CLI/Studio 只展示 models/backends；
- 更新 README Changelog、架构文档和 SVG；
- 全量门禁通过后把 RFC 和 RFC index 更新为 `Completed`。

## 10. 推荐提交顺序

不要把全部迁移压在一个提交中。推荐：

1. `docs(rfc): define model config transition dialect`
2. `feat(pipeline): parse model backend configuration`
3. `feat(validator): plan model backend materialization`
4. `feat(runtime): atomically materialize planned models`
5. `feat(onnx): implement tensor graph backend`
6. `feat(embedding): migrate bge embedding capability`
7. `feat(rerank): reuse onnx backend`
8. `feat(llm): add llama backend and qwen model`
9. `feat(multimodal): migrate ocr and asr models`
10. `refactor(runtime): remove legacy engine compatibility`
11. `docs(rfc): complete model backend decoupling`

每个提交至少执行受影响专项测试；每个阶段结束执行完整门禁。

## 11. 每阶段验收命令

```bash
./scripts/format.sh
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/alg_pipeline_tool catalog
./build/alg_pipeline_tool validate <迁移后的配置>
./build/alg_pipeline_tool plan <迁移后的配置>
./build/alg_demo --help
./scripts/run_all_tests.sh --full
```

阶段验收时额外检查：

```bash
# 禁止生产固定返回桩
rg -n "stub|fake|emulator|固定返回|Generated answer|OCR Stub" src/engine

# vendor 类型不得泄漏到 Core/Node/C ABI
rg -n "Ort::|onnxruntime|llama_|ggml_" include/core include/nodes src/core src/common_nodes include/company_alg_interface.h

# 新固定批路径必须使用唯一 Executor
rg -n "FixedBatchExecutor::Execute" src/engine src/common_nodes
```

## 12. 停止条件

出现以下任一情况时，不要继续下一阶段：

- 需要在生产代码中返回假结果才能让测试通过；
- Validator 失败后仍发生 Backend Load；
- Pipeline 失败后 Session 中存在部分 Model；
- Model 包含 vendor runtime 类型；
- Backend 根据 model type 分支处理 tokenizer/pooling/sampling；
- Node 重复实现 Model 预后处理；
- fake 出现在生产 Catalog；
- 全量 CTest 或六阶段门禁不通过。

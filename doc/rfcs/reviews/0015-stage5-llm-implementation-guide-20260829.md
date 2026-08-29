# RFC-0015 阶段 5：llama.cpp + Qwen LLM 可执行实施指南

- **编写日期**：2026-08-29
- **适用分支**：`feat/model-backend-decoupling-rfc`
- **前置提交**：`83df4df`（阶段 4 已验收通过）
- **阶段目标**：把 llama.cpp 的 vendor 执行资源与 Qwen 文本生成语义彻底拆开
- **完成后 RFC 状态**：仍为 `In Implementation`，不得标记 `Completed`

本文档是阶段 5 的实施和验收依据。请按任务编号顺序完成；不要在本阶段提前迁移 OCR/ASR，也不要删除仍由阶段 6 使用的 Mock Engine 或阶段 7 才统一清理的 Legacy 配置方言。

## 1. 最终交付形态

```text
LlmGenerateNode (Layer 3)
  -> ILlmModel::Generate(TextBatch, GenerateOptions)
     -> QwenCausalLmModel
        - Qwen ChatML template
        - prompt/context policy
        - prefill/decode loop
        - temperature + top-p sampling
        - end token + stop words + max_tokens
        - provenance
        -> ICausalLmSession
           -> LlamaCppSession
              - GGUF model/vocab
              - per-request context/KV state
              - token encode/decode primitive
              - Evaluate(tokens, state, logits)
```

必须满足两条硬边界：

1. `LlamaCppBackend` 不包含 chat template、采样、stop word、生成循环或业务回答；
2. `QwenCausalLmModel` 不 include `llama.h`，不持有 `llama_model`、`llama_context`、sampler 或其他 vendor handle。

生产路径禁止 emulator/fallback。GGUF 文件缺失、格式错误、vendor 初始化失败或运行时未编译时必须 fail-close，不得返回固定中文回答、业务 JSON 或伪造推理成功。

## 2. 当前基线

阶段 4 后已经具备：

- `ILlmModel` 与 `GenerateOptions`；
- `IBackendSession`、`ITokenCodec`、`ISequenceState`、`ICausalLmSession`；
- `ModelRegistry`、`BackendRegistry`、`ModelRuntimeFactory`；
- `FixedBatchExecutor` 中性的 `BatchPolicy + BatchSlice` 入口；
- `tests/support/inference/test_causal_lm_backend.*` 测试替身骨架。

仍需整改的旧实现：

- `LlmGenerateNode` 仍绑定 `ILlmEngine`；
- `LlamaCppEngine` 同时持有 vendor 资源、Chat/采样/生成循环和业务模拟回答；
- 真实 llama.cpp 配置仍使用 `engine_type + config` Legacy 方言；
- `test_qwen_engines_comparison.cpp` 依赖缺模型也成功的生产 emulator；
- `ModelManager` 尚无 `ILlmEngine -> ILlmModel` 过渡适配器，直接切换 Node 会破坏现有 Mock smoke Pipeline。

## 3. 范围

### 3.1 本阶段必须完成

- 固化 Causal LM protocol 的调用语义；
- 新增唯一的 `LlamaCppBackend`；
- 新增 `QwenCausalLmModel`；
- 将 `LlmGenerateNode` 改为 `ILlmModel`；
- 为阶段 6 前仍保留的 Legacy Mock LLM 增加窄兼容适配；
- 迁移所有真实 llama.cpp Pipeline 到 Model/Backend 方言；
- 删除旧 `LlamaCppEngine` 及其业务 emulator；
- 完成 sequence、context、sampling、stop、并发、配置、Pipeline 和 fail-close 测试。

### 3.2 本阶段不要做

- 不改 C ABI 输入输出结构；
- 不改 `LlmGenerateNode` 的 `prompt` / `text` 端口、Node Type、DAG 关系；
- 不迁移 OCR/ASR；
- 不删除 `IModelEngine`、`EngineFactory` 或 Legacy 方言；这些属于阶段 7；
- 不删除 `mock_npu_llm`；它随生产 Mock Engine 在阶段 6 统一迁出；
- 不提交真实 GGUF、大模型、llama.cpp 源码或预编译库；
- 不把平台、芯片或 device 字段塞进公共 Model 接口。

## 4. 文件清单

### 4.1 新增

```text
src/engine/backends/llama_cpp/llama_cpp_backend.h
src/engine/backends/llama_cpp/llama_cpp_backend.cpp
src/engine/models/qwen_causal_lm/qwen_causal_lm_model.h
src/engine/models/qwen_causal_lm/qwen_causal_lm_model.cpp
tests/test_llama_cpp_backend.cpp
tests/test_qwen_causal_lm_model.cpp
```

### 4.2 修改

```text
CMakeLists.txt
cmake/Tests.cmake
include/core/session_context.h
include/engine/backend_interface.h              # 只补文档化语义或必要的中性契约
include/contracts/inference_payloads.h          # 仅在确有必要时补中性 GenerateOptions
src/common_nodes/llm_generate_node.cpp
tests/support/inference/test_causal_lm_backend.*
tests/test_llm_generate_node.cpp
tests/test_model_backend_pipeline.cpp
tests/test_qwen_engines_comparison.cpp           # 建议重写并改名
configs/pipeline_entity_extract_llamacpp.json
configs/pipeline_doc_qa_onnx.json
configs/pipeline_doc_qa_rerank_real.json
doc/architecture.puml
README.md
```

### 4.3 删除

```text
src/engine/llama_cpp/llama_cpp_engine.h
src/engine/llama_cpp/llama_cpp_engine.cpp
```

删除前必须先完成真实 llama.cpp 配置迁移、Node 切换和测试重写。不得同时保留两份 llama.cpp vendor adapter。

## 5. S5-01：冻结 Causal LM protocol 语义

现有接口形状可以保留，但实现和测试必须遵循下列语义。

### 5.1 `ITokenCodec`

- `Encode(text, add_bos, tokens)`：成功时覆盖输出；失败时清空输出；空输出指针失败；
- `DecodeToken(token, piece)`：返回该 token 对应的原始字节片段，片段不要求单独构成完整 UTF-8；
- `IsEndToken(token)`：只判断模型 vocabulary 的 EOG/EOS 类 token，不解释 stop words；
- 所有方法 `noexcept`，vendor 异常不得越界。

### 5.2 `ISequenceState`

- 每个有效请求创建一个独立状态；
- 状态持有该请求的 context/KV cache，不得跨请求复用；
- 状态必须在对应 Session/Model 仍存活时使用；
- 创建失败返回空指针并提供 diagnostic。

### 5.3 `ICausalLmSession::Evaluate`

调用约定固定为：

```text
第一次调用：Evaluate(完整 prompt tokens, state, logits)  # prefill
后续调用：Evaluate({上一步生成 token}, state, logits)    # decode
```

- `tokens` 不能为空；
- 成功时 `logits` 是“最后一个输入位置之后的下一个 token”分布；
- 失败时清空 `logits`；
- Model 不得每轮重复提交完整历史；Backend 不得实现采样或生成循环；
- Session 返回的 `BatchPolicy` 在本阶段固定为 `{1, 0}`；一个 sequence 一次只服务一个请求。

### 5.4 必补测试

在 `tests/support/inference/test_causal_lm_backend.*` 增加可脚本化能力：

- 记录每次 `CreateSequence` 的唯一 state ID；
- 记录每个 state 的 Evaluate token 序列；
- 支持逐步返回指定 logits；
- 支持 Encode、Decode、CreateSequence、Evaluate 故障注入；
- 记录当前/最大并发 Evaluate 数；
- 支持非法空 logits、NaN/Inf logits 和越界 token 场景。

测试替身只编入测试 target，禁止进入生产 Catalog。

## 6. S5-02：实现 `LlamaCppBackend`

### 6.1 Definition

建议 Backend Definition 固定为：

```text
backend_type         = "llama_cpp"
supported_protocols = [kCausalLm]
concurrency          = kSerialized
```

建议 `backend_config`：

| 字段 | 类型 | 默认值 | 约束 | 归属 |
| :--- | :--- | :--- | :--- | :--- |
| `context_size` | integer | `2048` | `16..1048576` | Backend |
| `decode_batch_size` | integer | `512` | `1..context_size` | Backend |
| `n_threads` | integer | `0` | `0..1024`，0 表示 runtime 默认 | Backend |
| `n_threads_batch` | integer | `0` | `0..1024` | Backend |
| `n_gpu_layers` | integer | `0` | `0..1048576` | Backend |
| `use_mmap` | boolean | `true` | - | Backend |
| `use_mlock` | boolean | `false` | - | Backend |

不要继续使用旧 Engine 的 `max_batch_size` 和 `max_seq_len` 名称。请求批处理由 Model/Executor 管理，Backend 的 `decode_batch_size` 是 llama.cpp token decode 容量，不是业务请求 batch。

### 6.2 所有权

推荐所有权结构：

```text
LlamaCppSession
  owns llama_model*
  exposes vocabulary through LlamaCppTokenCodec
  creates LlamaCppSequenceState
    owns llama_context* / KV state for exactly one request
```

要求：

- `LlamaCppBackend::Load` 校验 model path 存在、为常规文件且 vendor 加载成功；
- Provider 本身可在 `Load` 返回后销毁，Session 必须独立持有全部所需资源；
- Sequence 析构先释放 context，Session 析构再释放 model；
- llama 全局 runtime 使用线程安全的一次性/引用计数 RAII，禁止每次 Load 无配对地初始化；
- vendor 类型只出现在 `.cpp` 或私有 PIMPL，公共头不得 include `llama.h`；
- `CreateSequence` 根据 Session 配置创建独立 context；
- `Evaluate` 必须验证 state 类型、token 非空、context limit 和 vendor 返回码；
- `kSerialized` 必须在运行时通过 Session 内互斥锁落实，不能只写在 Definition 中；
- diagnostic 只记录路径、阶段和错误，不记录完整 prompt。

### 6.3 编译开关

- `HAVE_LLAMACPP` 存在时才注册 `llama_cpp` Backend；
- `ENABLE_LLAMACPP=OFF` 时工程仍完整编译，`BackendRegistry` 中不得出现 `llama_cpp`；
- CMake 只把 vendor include/link 细节赋给生产库的 Backend 实现；
- 不创建同名占位 Backend，也不回退到 emulator。

### 6.4 Backend 测试

至少覆盖：

- Definition/Registry 正确；
- 缺文件、目录路径、损坏 GGUF、非法配置均 Load 失败；
- 编译关闭时 Backend 不注册；
- Sequence 独立创建和析构；
- 错误 state 类型、空 tokens、context overflow、vendor decode 错误；
- 多线程调用下 `max_active_evaluate == 1`；
- Session/Sequence/vendor 资源只析构一次；
- 源码不存在业务关键词分支、固定回答、采样器或 ChatML 模板。

真实 GGUF 测试通过环境变量注入，不提交权重：

```bash
LLM_EDGEFLOW_TEST_GGUF_MODEL=/absolute/path/to/qwen.gguf \
  ./build/edgeflow_test_core_runner \
  '--gtest_filter=LlamaCppBackendTest.RealGgufLoadCodecAndEvaluate'
```

标准 CI 可在环境变量缺失时明确 `GTEST_SKIP` 这一个外部资产用例；阶段 5 人工验收记录必须至少保存一次真实 GGUF PASS 证据。

## 7. S5-03：实现 `QwenCausalLmModel`

### 7.1 Model Definition

```text
model_type       = "qwen_causal_lm"
capability       = "llm"
required_protocol = kCausalLm
concurrency      = kConcurrent
```

Model 自身每个请求只使用局部 state，可声明 `kConcurrent`；最终有效并发仍会因 `LlamaCppBackend=kSerialized` 收紧为 serialized。

建议 `model_config`：

| 字段 | 类型 | 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `chat_template` | string enum | `qwen_chatml` | 本阶段只接受 `qwen_chatml` |
| `system_prompt` | string | `""` | 可选系统提示，不属于 Backend |
| `add_bos` | boolean | `false` | 传递给 `ITokenCodec::Encode` |
| `random_seed` | integer | `-1` | `-1` 为随机种子；非负值供可复现测试/部署 |

不要把 `temperature`、`top_p`、`max_tokens` 或 `stop_words` 放入 Model 配置；它们属于每次请求的 `GenerateOptions`，由 Node 传入。

### 7.2 ChatML

统一生成：

```text
[可选] <|im_start|>system\n{system_prompt}<|im_end|>\n
<|im_start|>user\n{prompt}<|im_end|>\n
<|im_start|>assistant\n
```

ChatML 拼装只在 Qwen Model 中。Backend、Node、Pipeline 和配置模板均不得重复拼装。

### 7.3 单请求生成算法

按以下顺序实现 `GenerateOne`：

1. 校验 prompt、options、输出指针；
2. 应用 Qwen ChatML；
3. `TokenCodec.Encode`；
4. 校验 prompt token 数严格小于 `MaxContextTokens()`；
5. 创建独立 `ISequenceState`；
6. `Evaluate(prompt_tokens)` 完成 prefill；
7. 对最多 `min(max_tokens, context_remaining)` 步执行：
   - 校验 logits 非空且所有值有限；
   - `temperature <= 0.01` 时 greedy；否则稳定 softmax 后做 top-p；
   - 先检查 `IsEndToken`；
   - `DecodeToken` 并追加原始字节片段；
   - 在完整累计文本上检测 stop words，命中时截断 stop word 本身并结束；
   - 未结束则 `Evaluate({sampled_token})`；
8. 清理末尾不完整 UTF-8 序列；
9. 成功返回文本；任一步失败均清空输出。

采样实现要求：

- softmax 前减去最大 logit，防止指数溢出；
- `top_p` 必须在 `(0, 1]`；保留按概率降序、累计概率首次达到 `top_p` 的最小集合；
- 重新归一化候选概率后再采样；
- 非有限 logits、空候选集合、采样 token 越界全部 fail-close；
- 固定 `random_seed` 时结合 `(req_id, sub_id)` 派生每请求 RNG，避免并发共享可变 RNG。

stop word 必须支持跨 token piece 命中。例如 pieces 为 `"ST"`、`"OP"`，stop word 为 `"STOP"` 时输出应在 `S` 之前截断。

### 7.4 Batch 与 provenance

`ILlmModel::Generate` 必须调用新版：

```cpp
FixedBatchExecutor::Execute<std::string, std::string>(
    prompts, session->GetBatchPolicy(), callback, outputs);
```

- 不使用语义 dummy 字符串；
- callback 只访问 `BatchSlice` 指向的有效输入；
- 任一请求失败时整批输出回滚；
- `(req_id, sub_id)` 只能由 Executor 从输入继承；
- 创建期拒绝不适合 Causal LM 的非法 BatchPolicy；本阶段 `LlamaCppSession` 必须返回 `{1, 0}`。

### 7.5 Model 测试矩阵

至少覆盖：

- 非 Causal Session、空 Session、非法 Model config 创建失败；
- ChatML 与 add_bos 参数准确；
- prompt 等于/超过 context limit；
- context 剩余量小于 `max_tokens` 时正确收敛；
- greedy、temperature、top-p 边界和固定 seed 可复现；
- EOG、max_tokens、单 piece stop、跨 piece stop；
- Encode/Decode/CreateSequence/Evaluate 故障全回滚；
- 空、NaN、Inf logits fail-close；
- 多请求使用不同 sequence state；
- 输出顺序和 provenance 保持；
- 第二个请求失败时第一个结果不泄漏；
- 并发调用无共享 RNG/state 数据竞争；
- Model 源码不包含 `llama_` vendor 标识或 backend 名称分支。

## 8. S5-04：切换 `LlmGenerateNode`

### 8.1 Node 修改

将：

```cpp
TraceableUnaryInferenceNode<ILlmEngine, std::string, std::string>
```

改为直接基于 `ModelBoundNode<ILlmModel>`，或使用适合批接口的浅基类。核心调用固定为：

```cpp
return model()->Generate(prompts, generate_options_, outputs);
```

要求：

- 端口、Node Type、错误码、`model_capability="llm"` 和 `bind_model` 不变；
- `GenerateOptions` 使用中性 DTO；
- Node 只解析每请求生成选项，不实现 ChatML/采样/stop；
- 为 Node Definition 增加 `stop_words` 数组字段；Init 时验证每项均为非空字符串；
- `temperature`、`top_p`、`max_tokens` 继续由 Node schema 校验；建议把 `top_p` 的有效范围收紧为 `(0, 1]`，运行时仍二次校验。

### 8.2 Legacy 兼容适配

在 `include/core/session_context.h` 增加 `detail::LegacyLlmEngineAdapter : ILlmModel`，行为仅限：

- 把中性 `GenerateOptions` 逐字段复制到 `ILlmEngine::GenerateOption`；
- 调用旧 `InferTraceableBatch`；
- 校验数量、顺序和 provenance；
- 失败时清空输出。

并在 `ModelManager::GetModel<ILlmModel>` 中按需创建该 adapter。它只用于阶段 6 前保留的 `mock_npu_llm` Pipeline，不得持有 ChatML、采样或业务回答逻辑。阶段 7 随 Legacy Engine 一并删除。

### 8.3 Node 测试

- 使用测试 `ILlmModel` 验证 options 原样传递；
- 验证 `stop_words` 解析；
- 验证缺输入、错误模型 capability、Model 失败时 Node fail-close；
- 验证 Legacy Mock LLM 仍可通过适配器运行 smoke；
- 禁止继续用 `ILlmEngine` 作为 Node 的直接模板参数。

## 9. S5-05：迁移配置

本阶段迁移下列真实 llama.cpp 模型项：

1. `configs/pipeline_entity_extract_llamacpp.json`
2. `configs/pipeline_doc_qa_onnx.json`
3. `configs/pipeline_doc_qa_rerank_real.json`

迁移模板：

```json
{
  "model_id": "llm_model_llamacpp",
  "capability": "llm",
  "model_type": "qwen_causal_lm",
  "backend": "llama_cpp",
  "model_path": "./models/qwen2.5_1.5b_instruct_q4_k_m.gguf",
  "model_config": {
    "chat_template": "qwen_chatml",
    "system_prompt": "",
    "add_bos": false,
    "random_seed": -1
  },
  "backend_config": {
    "context_size": 1024,
    "decode_batch_size": 512,
    "n_threads": 0,
    "n_threads_batch": 0,
    "n_gpu_layers": 0,
    "use_mmap": true,
    "use_mlock": false
  }
}
```

迁移规则：

- 同一个 model object 中不得残留 `engine_type` 或 `config`；
- Node 的 `bind_model`、ports、DAG 完全不动；
- Mock LLM 配置暂时保持 Legacy 方言，通过 `LegacyLlmEngineAdapter` 兼容；
- 不在 Validator、Pipeline 或 RuntimeFactory 中增加 `llama_cpp`/`qwen` 名称分支；
- 三个配置必须同时通过 `validate` 和 `plan`；
- 切换 Backend 只能改模型项中的 `backend + backend_config + model_path`，不能改 Node。

执行：

```bash
for config in \
  configs/pipeline_entity_extract_llamacpp.json \
  configs/pipeline_doc_qa_onnx.json \
  configs/pipeline_doc_qa_rerank_real.json; do
  ./build/alg_pipeline_tool validate "$config"
  ./build/alg_pipeline_tool plan "$config"
done
```

## 10. S5-06：删除旧实现并重写回归测试

完成 Backend、Model、Node 和配置迁移后：

1. 从 `CMakeLists.txt` 删除 `llama_cpp_engine.cpp`；
2. 删除 `src/engine/llama_cpp/llama_cpp_engine.*`；
3. 确认生产 Catalog 不再有 legacy `llama_cpp` EngineDefinition，只保留同名 BackendDefinition；
4. 重写 `test_qwen_engines_comparison.cpp`，不得再比较两个生产 emulator 的固定业务回答；
5. 将 UTF-8 截断测试迁到 `QwenCausalLmModel`；
6. C ABI/Pipeline 切换测试使用受控 test backend/model，或在显式真实 GGUF 环境下运行；
7. 缺 GGUF 必须断言 Create 失败，不能再断言成功；
8. 增加源码边界静态门禁。

建议增加静态断言脚本或测试，至少检查：

```text
src/engine/backends/llama_cpp/ 不 include Node/Pipeline/AlgContext
src/engine/backends/llama_cpp/ 不含 ChatML、top_p、stop_words、业务固定回答
src/engine/models/qwen_causal_lm/ 不 include llama.h
src/engine/models/qwen_causal_lm/ 不按 BackendType 分支
src/common_nodes/llm_generate_node.cpp 不引用 ILlmEngine
生产源码不存在 LlamaCppEngine 注册
```

## 11. 建议实施顺序与提交粒度

按下列顺序可以保证每一步可编译：

1. `test(causal-lm): freeze protocol and scripted fake session`
2. `feat(backend): add fail-closed llama.cpp causal session`
3. `feat(model): add qwen causal LM generation semantics`
4. `refactor(node): bind LlmGenerateNode to ILlmModel`
5. `refactor(config): migrate real llama.cpp pipelines`
6. `refactor(engine): remove legacy LlamaCppEngine emulator`
7. `test(llm): close pipeline concurrency and real GGUF gates`
8. `docs(rfc): record stage5 acceptance evidence`

不要在测试尚未证明新路径可运行时先删除旧实现，也不要把所有逻辑堆在一个不可审查的大提交中。

## 12. 定向验收矩阵

| 类别 | 必须证明 |
| :--- | :--- |
| Protocol | prefill/decode 增量语义、独立 state、错误清空 |
| Backend | 真实 GGUF 加载、codec、context/KV、Evaluate、析构、serialized |
| Model | ChatML、context、sampling、stop、UTF-8、provenance、全回滚 |
| Node | `ILlmModel`、options 传递、Legacy Mock 兼容、端口不变 |
| Validator | Qwen + llama protocol 匹配，非法组合加载前拒绝 |
| Pipeline | 新配置 Build/Execute；模型失败无部分注册 |
| Build matrix | llama.cpp ON/OFF 均能配置、编译、CTest |
| Static boundary | vendor 只在 Backend，生成语义只在 Model |
| Production safety | 缺模型/损坏模型/未编译 runtime 均 fail-close，无 emulator |

建议新增或调整的 CTest：

```text
LlamaCppBackendTest
QwenCausalLmModelTest
LlmGenerateNodeTest
ModelBackendPipelineTest（causal LM cases）
QwenModelBackendIntegrationTest
LlmArchitectureBoundaryTest
```

## 13. 完整验证命令

所有使用 ccache 的命令在受限环境中可显式指定可写目录：

```bash
export CCACHE_DIR=/private/tmp/llm-edgeflow-ccache
```

### 13.1 格式、构建、定向测试

```bash
./scripts/format.sh
./scripts/format.sh --check
git diff --check

cmake -S . -B build -DENABLE_LLAMACPP=ON -DENABLE_ONNXRUNTIME=ON
cmake --build build -j4

./build/edgeflow_test_core_runner \
  '--gtest_filter=LlamaCppBackendTest.*:QwenCausalLmModelTest.*:ModelBackendPipelineTest.*'

./build/edgeflow_test_nodes_runner \
  '--gtest_filter=LlmGenerateNodeTest.*'
```

### 13.2 llama.cpp OFF 独立构建

```bash
cmake -S . -B build-no-llama-stage5 \
  -DENABLE_LLAMACPP=OFF \
  -DENABLE_ONNXRUNTIME=ON
cmake --build build-no-llama-stage5 -j4
ctest --test-dir build-no-llama-stage5 -j4 --output-on-failure
```

必须断言 `BackendRegistry` 中没有 `llama_cpp`，而 Mock smoke Pipeline 仍通过 Legacy adapter。

### 13.3 全量门禁

```bash
ctest --test-dir build -j4 --output-on-failure
LLM_EDGEFLOW_JOBS=4 ./scripts/run_all_tests.sh --full
LLM_EDGEFLOW_SANITIZERS=undefined ./scripts/run_sanitizers.sh --fast
./scripts/check_architecture_docs.sh
./scripts/render_architecture_diagrams.sh --check
```

### 13.4 真实 GGUF 条件验收

```bash
LLM_EDGEFLOW_TEST_GGUF_MODEL=/absolute/path/to/qwen.gguf \
  ./build/edgeflow_test_core_runner \
  '--gtest_filter=LlamaCppBackendTest.RealGgufLoadCodecAndEvaluate:QwenModelBackendIntegrationTest.RealGgufGenerate'
```

真实测试至少断言：

- Backend Load 成功；
- prompt Encode 非空；
- 创建 sequence 成功；
- prefill 与至少一步 decode 成功；
- logits 非空且有限；
- Model 输出为有效 UTF-8；
- 资源销毁后无 sanitizer 错误。

## 14. 阶段 5 通过标准

只有全部勾选后才可请求阶段 5 验收：

- [ ] `LlmGenerateNode` 只依赖 `ILlmModel`；
- [ ] `QwenCausalLmModel` 完整拥有 ChatML、context、sampling、stop 和生成循环；
- [ ] `LlamaCppBackend` 只拥有 GGUF/vocab/context/KV/Evaluate vendor 资源；
- [ ] Model 不 include vendor 头，Backend 不 include Node/Pipeline/业务载荷实现；
- [ ] 每个请求创建独立 sequence state；
- [ ] Backend 声明并落实 serialized 并发；
- [ ] LLM 批处理经过新版 `FixedBatchExecutor`，provenance 正确且失败全回滚；
- [ ] 三个真实 llama.cpp 配置迁移到 Model/Backend 方言并通过 validate/plan；
- [ ] Legacy Mock LLM smoke 仅通过窄 adapter 兼容；
- [ ] 旧 `LlamaCppEngine`、生产 emulator 和固定业务回答全部删除；
- [ ] 缺模型、损坏模型、runtime OFF 全部 fail-close；
- [ ] sequence、context、sampling、stop、并发和 UTF-8 测试完整；
- [ ] 至少一次真实 GGUF 条件测试通过并记录模型标识/哈希，不提交权重；
- [ ] llama.cpp ON/OFF 独立 CTest 100% 通过；
- [ ] 六阶段完整回归、UBSan、格式、架构门禁全部通过；
- [ ] README/architecture 更新，RFC-0015 仍保持 `In Implementation`；
- [ ] 未提前实施阶段 6/7。

## 15. 验收记录模板

完成后在同目录新增 `0015-stage5-llm-acceptance-YYYYMMDD.md`，记录：

```text
实现提交：
真实 GGUF 测试模型标识与 SHA-256：
LLM 定向测试：x/x PASS，skip 数：
llama.cpp ON CTest：x/x PASS
llama.cpp OFF CTest：x/x PASS
完整六阶段回归：x/x PASS
Sanitizer：x/x PASS
配置 validate/plan：3/3 PASS
架构边界扫描：PASS
阶段结论：PASS / FAIL
剩余问题：
```

阶段 5 通过后进入阶段 6；RFC-0015 只有阶段 6 和阶段 7 也完成、所有门禁全绿后才能标记为 `Completed` 并进入合并流程。

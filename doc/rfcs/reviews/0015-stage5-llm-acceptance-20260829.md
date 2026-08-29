# RFC-0015 阶段 5：llama.cpp + Qwen LLM 验收结论

- **验收日期**：2026-08-29
- **适用分支**：`feat/model-backend-decoupling-rfc`
- **实施依据**：[0015-stage5-llm-implementation-guide-20260829.md](0015-stage5-llm-implementation-guide-20260829.md)
- **阶段结论**：**PASS，可以进入阶段 6**
- **RFC 状态**：继续保持 `In Implementation`

## 1. 结论

阶段 5 已完成 llama.cpp vendor runtime 与 Qwen 文本生成语义的真实拆分，不再存在旧
`LlamaCppEngine` 组合实现或缺权重仍返回业务固定回答的生产 emulator。

最终调用链为：

```text
LlmGenerateNode
  -> ILlmModel
     -> QwenCausalLmModel
        -> ICausalLmSession
           -> LlamaCppBackend / LlamaCppSession
              -> llama.cpp GGUF runtime
```

验收确认：

- Node 只依赖 `ILlmModel`；
- Qwen Model 独占 ChatML、temperature/top-p、stop word、生成循环和 UTF-8 收尾；
- llama.cpp Backend 只持有 GGUF、vocab、context/KV、token codec 和 logits Evaluate；
- 每请求独立 sequence，Backend Evaluate 由互斥锁落实 serialized 契约；
- LLM 批处理使用新版 `FixedBatchExecutor`，保持 provenance，失败全量回滚；
- 三份真实 llama.cpp 配置已迁移到 `model_type + backend` 方言；
- `ENABLE_LLAMACPP=OFF` 时 Backend 不注册，Mock LLM smoke 通过窄 Legacy adapter 保持可用；
- 缺文件、目录、未知配置、runtime 未编译均 fail-close。

## 2. 关键整改与验收发现

### 2.1 已删除生产伪成功路径

已删除：

```text
src/engine/llama_cpp/llama_cpp_engine.h
src/engine/llama_cpp/llama_cpp_engine.cpp
tests/test_qwen_engines_comparison.cpp
```

旧测试依赖“模型文件不存在仍成功”的 emulator，现已替换为脚本化 Causal LM 单测、
fail-close Backend 测试和显式真实 GGUF 条件测试。

### 2.2 真实 GGUF 验收发现并修复 Metal 隐式依赖

首次物理前向时，权重与词表加载成功，但 llama.cpp 在无可用 Metal command queue 的环境
中创建 context 失败，即使 `n_gpu_layers=0`。现新增：

```text
LLM_EDGEFLOW_LLAMACPP_METAL=OFF  # 默认，确定性的 CPU 基线
LLM_EDGEFLOW_LLAMACPP_METAL=ON   # 部署环境显式确认 Metal 可用时开启
```

修复后，同一 GGUF 的 Backend prefill、增量 decode、Qwen Model 生成和 C ABI Pipeline 均
通过。该修复避免生产部署因隐式硬件探测而不可用。

### 2.3 当前 llama.cpp API 字段收敛

当前 pinned 构建实际支持 `check_tensors`，不再使用旧实施建议中的 `use_mmap/use_mlock`
字段。Backend Definition 与三份配置均已按实际 API 收敛，并对未知字段严格拒绝。

## 3. 配置迁移证据

以下配置均完成 `validate + plan`：

1. `configs/pipeline_entity_extract_llamacpp.json`
2. `configs/pipeline_doc_qa_onnx.json`
3. `configs/pipeline_doc_qa_rerank_real.json`

结果：**3/3 validate PASS，3/3 plan PASS**。

配置中的 LLM 模型统一为：

```json
{
  "capability": "llm",
  "model_type": "qwen_causal_lm",
  "backend": "llama_cpp",
  "model_config": {
    "chat_template": "qwen_chatml",
    "add_bos": false,
    "random_seed": -1
  },
  "backend_config": {
    "context_size": 512,
    "decode_batch_size": 512,
    "n_gpu_layers": 0,
    "check_tensors": false
  }
}
```

## 4. 测试证据

### 4.1 定向测试

- `QwenCausalLmModelTest`：ChatML、独立 state、greedy、top-p、跨 token stop、context、
  NaN/空 logits、批次回滚、provenance、UTF-8；
- `LlamaCppBackendTest`：Registry/Definition、缺文件、目录、未知字段、vendor 头隔离、真实
  GGUF codec/Evaluate/Model Generate；
- `ModelBackendPipelineTest`：13/13 PASS；
- `LlmGenerateNodeTest`：2/2 PASS；
- LLM 架构边界扫描：PASS。

### 4.2 真实 GGUF

```text
模型：qwen2.5-0.5b-instruct-q4_k_m.gguf
SHA-256：74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db
Backend + codec + prefill/decode + Model Generate：1/1 PASS
真实模型 E2E（单条、批量 provenance、C ABI Pipeline）：3/3 PASS
```

真实权重仅作为本地条件测试资产，未纳入提交。

### 4.3 构建与回归矩阵

| 门禁 | 结果 |
| :--- | :--- |
| llama.cpp ON + ONNX Runtime ON CTest | 86/86 PASS |
| llama.cpp OFF + ONNX Runtime ON CTest | 86/86 PASS |
| `run_all_tests.sh --full` 六阶段门禁 | 86/86 PASS，6/6 stages PASS |
| UBSan fast | 82/82 PASS |
| 格式化与 `git diff --check` | PASS |
| 架构文档与 SVG render/check | PASS |
| LayerGuard（含 LLM vendor/semantic 边界） | PASS |

## 5. 阶段 5 完成清单

- [x] `LlmGenerateNode` 只依赖 `ILlmModel`；
- [x] `QwenCausalLmModel` 拥有 ChatML、sampling、stop 和生成循环；
- [x] `LlamaCppBackend` 只拥有 vendor runtime 与中性 Causal LM 原语；
- [x] Model 不 include vendor 头，Backend 不依赖 Node/Pipeline/AlgContext；
- [x] 每请求独立 sequence，serialized Evaluate 契约落实；
- [x] `FixedBatchExecutor`、provenance 与错误全回滚已验证；
- [x] 三份真实配置迁移并通过 validate/plan；
- [x] Legacy Mock LLM 只通过窄 adapter 过渡；
- [x] 旧 `LlamaCppEngine` 和生产 emulator 已删除；
- [x] runtime ON/OFF、真实 GGUF、全量回归、UBSan、格式与架构门禁全部通过；
- [x] README 之外的阶段架构文档已同步；README 最终 changelog 在 RFC 全部收口时统一更新。

## 6. 下一阶段边界

阶段 6 只处理 OCR、ASR 与测试替身迁移：

- 建立 PPOCR/Paraformer typed Model；
- 将 OCR/ASR Node 从 Legacy Engine 切到 `IOcrModel` / `IAsrModel`；
- 把生产 Mock Engine/业务固定响应迁出生产 Catalog，替换为 tests/support fake；
- 保持 C ABI、Node Type、端口、Blackboard Key 和业务 DAG 不变。

阶段 6/7 尚未完成，因此本阶段不得把 RFC-0015 或 RFC 索引改为 `Completed`，也不得合入
`main`。

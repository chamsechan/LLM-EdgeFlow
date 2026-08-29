# RFC-0015 阶段 4：Rerank 复用 ONNX Backend 可执行实施指南

- **制定日期**：2026-08-29
- **关联 RFC**：`doc/rfcs/0015-model-capability-backend-decoupling.md`
- **关联分支**：`feat/model-backend-decoupling-rfc`
- **实施起点**：`7ea12b3`（阶段 3 验收通过提交）
- **当前状态**：Ready for Implementation
- **阶段目标**：实现 `BgeRerankerModel`，让 Embedding 与 Rerank 复用唯一
  `OnnxRuntimeBackend`，并保持 `TextRerankNode` 的端口、分组、排序和 top-k 行为不变

> 本文是阶段 4 的唯一执行清单。实施时按第 5 节顺序推进，每完成一个步骤立即运行该步骤
> 的定向测试；不要先删除旧路径，再补实现和测试。

## 0. 开始前先确认

执行：

```bash
git branch --show-current
git log -1 --oneline
git status --short
```

预期：

- 分支为 `feat/model-backend-decoupling-rfc`；
- 历史中包含阶段 3 提交 `7ea12b3`；
- 开始编码前工作区只允许存在你明确知道来源的阶段 4 修改；
- RFC-0015 继续保持 `In Implementation`，不得提前改为 `Completed`。

如果阶段 3 完整回归证据失效，先回到阶段 3 验收文档处理，不要在阶段 4 顺手修改。

## 1. 阶段 4 完成后的唯一目标形态

```text
TextRerankNode
    │ QueryCandidatesBatch / ScoreBatch
    ▼
IRerankModel
    ▼
BgeRerankerModel
    │ TensorMap（input_ids / attention_mask / token_type_ids）
    ▼
ITensorGraphSession
    ▼
OnnxRuntimeBackend（与 BgeEmbeddingModel 共用同一个 Backend Creator）
    ▼
ONNX Runtime
```

旧 Mock NPU 配置在过渡期走以下兼容路径：

```text
TextRerankNode -> IRerankModel -> LegacyRerankEngineAdapter
                                  -> IRerankEngine
```

兼容适配器只能位于 `ModelManager` 的旧方言边界。`TextRerankNode` 不得引用
`IRerankEngine::PairInput`，`BgeRerankerModel` 不得引用 ONNX Runtime 类型。

### 1.1 必须达到

- 生产 Catalog 注册 `bge_reranker`，capability 为 `rerank`，协议为 `tensor_graph`；
- `BgeEmbeddingModel` 和 `BgeRerankerModel` 都只依赖 `ITensorGraphSession`；
- 两者都通过 BackendRegistry 中唯一的 `onnxruntime` Creator 加载执行；
- pair tokenizer、Tensor 构造、logit 校验和 score decode 位于 Model；
- 输入分组、按 `req_id` 排序和 top-k 位于 Node；
- 所有批推理经过新版 `FixedBatchExecutor::Execute`；
- 删除 `OnnxRerankEngine` 的注册、源文件和构建引用；
- 真实 ONNX fixture 至少一次非 skip PASS；
- ONNX ON/OFF、配置 validate/plan、Node 回归和完整六阶段门禁通过。

### 1.2 本阶段禁止做

- 不实现阶段 5 的 llama.cpp / Qwen；
- 不删除 `IModelEngine`、`IRerankEngine`、`EngineFactory` 或 Mock NPU Rerank；
- 不把 Mock NPU 实现伪装成新的生产 Backend/Model；
- 不迁移 OCR、ASR、LLM 配置；
- 不把排序、top-k 或 `RankedTextBatch` 组装下沉到 Model/Backend；
- 不在 `OnnxRuntimeBackend` 中增加 `if (rerank)` 或 score/sigmoid 逻辑；
- 不修改 C ABI、Blackboard Key、Node 端口或业务 DAG；
- 不因本机 ASan 不可用而跳过 UBSan、ONNX OFF 和完整回归。

## 2. 固定契约：实现前不要自行改方言

### 2.1 Model 接口

沿用已有接口，不新增通用 `Infer(void*)`：

```cpp
class IRerankModel : public IModel {
 public:
  virtual int Score(const QueryCandidatesBatch& inputs,
                    ScoreBatch* outputs) noexcept = 0;
};
```

`Score` 必须满足：

- `outputs == nullptr`：返回稳定非零错误码；
- 进入函数立即 `outputs->clear()`；
- 空输入：返回 0 和空输出，不调用 Backend；
- 成功：输出数量与输入完全一致，并逐项保持 `(req_id, sub_id)`；
- 任一批失败、输出不合法或异常：清空全部输出，不保留前批结果；
- 所有异常都在 `noexcept` 边界内转换为错误码。

### 2.2 Pair 编码

统一使用 BERT Cross-Encoder 格式：

```text
[CLS] query_tokens [SEP] candidate_tokens [SEP] [PAD]...
token_type_ids: 0......................0 1................1 0...
attention_mask: 1......................................... 0...
```

固定规则：

- `max_length >= 3`；
- query 和 candidate 分别执行现有 Basic + WordPiece 规则；
- 超长时使用 deterministic longest-first：每次从较长一侧删除末尾 token，直到
  `query_size + candidate_size <= max_length - 3`；长度相同时优先截断 candidate；
- `[CLS]`、第一个 `[SEP]` 的 token type 为 0；candidate 和第二个 `[SEP]` 为 1；
- padding 的 id 为 `[PAD]`，mask 为 0，token type 为 0；
- 非法 UTF-8、未加载 tokenizer、空输出指针必须 fail-closed；
- 空 query/candidate 可以编码，但仍必须生成三个特殊 token。

### 2.3 输入 Tensor

Model 构造以下 `int64` Tensor，运行时 shape 必须是
`[BatchSlice.execution_count, max_length]`：

| 名称 | 必需性 | 说明 |
| :--- | :--- | :--- |
| `input_ids` | 必需 | pair token id |
| `attention_mask` | 必需 | 非 padding 为 1 |
| `token_type_ids` | 按 Session metadata | Session 声明时才传入 |

Model 创建阶段必须检查 Session 输入至少包含 `input_ids` 和 `attention_mask`。Session 如果
声明 Model 不认识的必需输入，应创建失败并写 diagnostic，不能等到首个请求才暴露。

### 2.4 输出 Tensor 与 score decode

阶段 4 只接受 `float32`：

- `[batch]`；或
- `[batch, 1]`。

其他 rank、第二维不为 1、batch 不符、零/负维度、byte size 不精确、未对齐、NaN、Inf、
缺失 `output_name` 均失败。读取数据必须使用 `GetTensorData<float>`，禁止未检查的
`reinterpret_cast`。

配置 `score_activation` 固定支持：

- `sigmoid`（默认）：使用数值稳定实现，避免 `exp()` 溢出；
- `identity`：保留有限原始 logit。

稳定 sigmoid：

```cpp
if (x >= 0.0f) {
  return 1.0f / (1.0f + std::exp(-x));
}
const float e = std::exp(x);
return e / (1.0f + e);
```

不要在 Backend 中实现 activation。

### 2.5 ModelDefinition

注册固定为：

| 字段 | 值 |
| :--- | :--- |
| `model_type` | `bge_reranker` |
| `capability` | `rerank` |
| `required_protocol` | `ExecutionProtocol::kTensorGraph` |
| `concurrency` | 与实际 Session 一致；当前 ONNX 为 `kConcurrent` |

`model_config` 字段：

| 字段 | 类型 | 必需 | 默认/范围 |
| :--- | :--- | :--- | :--- |
| `tokenizer_file` | string | 否 | `vocab.txt`，不能为空 |
| `do_lower_case` | bool | 否 | `true` |
| `max_length` | integer | 否 | `512`，范围 3..4096 |
| `output_name` | string | 否 | `logits`，不能为空 |
| `score_activation` | enum string | 否 | `sigmoid`，允许 `sigmoid/identity` |
| `max_batch_size` | integer | 否 | `4`，范围 1..1024 |

不要把 `device_id`、线程数、Execution Provider 等 Backend 字段加入 ModelDefinition。

## 3. 文件级改造清单

### 3.1 新增

- `src/engine/models/bge_reranker/bge_reranker_model.h`
- `src/engine/models/bge_reranker/bge_reranker_model.cpp`
- `tests/test_onnx_and_reranker_model.cpp`

### 3.2 修改

- `src/engine/models/bge_embedding/bert_wordpiece_tokenizer.h`
- `src/engine/models/bge_embedding/bert_wordpiece_tokenizer.cpp`
- `include/core/session_context.h`
- `src/common_nodes/text_rerank_node.cpp`
- `scripts/generate_test_onnx_model.py`
- `CMakeLists.txt`
- `cmake/Tests.cmake`
- `configs/pipeline_cross_rerank.json`
- `configs/pipeline_doc_qa_rerank_real.json`
- `tests/test_text_rerank_node.cpp`
- `tests/test_rerank_refine_node.cpp`
- `tests/test_common_nodes.cpp`（仅在旧方言适配回归需要时修改）
- `tests/test_framework_core.cpp`
- `tests/test_catalog_contract_ssot.cpp`
- `tests/test_pipeline_config.cpp`、`tests/test_pipeline_studio.cpp`（ONNX OFF 条件矩阵）
- 本文和阶段 4 验收文档（完成实现后记录证据）

可选整理：若移动 tokenizer 到 `src/engine/models/common/`，必须在独立提交中只做路径移动，
先保证现有 Embedding 测试零行为变化，再增加 `EncodePair`。不要复制一份 tokenizer。

### 3.3 删除

- `src/engine/onnx/onnx_rerank_engine.h`
- `src/engine/onnx/onnx_rerank_engine.cpp`
- `CMakeLists.txt` 中对应源文件项

本阶段不要删除 `src/engine/onnx/onnx_embedding_engine.*`；它的最终兼容收口属于后续阶段。

## 4. 配置迁移的精确结果

只迁移仍使用 `engine_type: onnx_rerank` 的两个 JSON：

- `configs/pipeline_cross_rerank.json`
- `configs/pipeline_doc_qa_rerank_real.json`

每个目标模型改为：

```json
{
  "model_id": "保持原值",
  "capability": "rerank",
  "model_type": "bge_reranker",
  "backend": "onnxruntime",
  "model_path": "保持原 ONNX 路径",
  "model_config": {
    "tokenizer_file": "vocab.txt",
    "do_lower_case": true,
    "max_length": 512,
    "output_name": "logits",
    "score_activation": "sigmoid",
    "max_batch_size": 4
  },
  "backend_config": {
    "max_batch_size": 4,
    "intra_op_num_threads": 2,
    "inter_op_num_threads": 1,
    "graph_optimization_level": "all"
  }
}
```

约束：

- 单个 model entry 内禁止新旧字段混用；删除 `engine_type` 和 `config`；
- `model_id` 和 Node 的 `bind_model` 不变；
- `.conf` 中 `model_paths` key 不变；
- `pipeline_doc_qa_rerank.json`、`pipeline_dialogue_audit.json` 的 Mock NPU Rerank 暂不迁移，
  通过第 5.2 节兼容适配器保持回归；
- ONNX OFF 时目标 ONNX 配置允许在专属条件测试中跳过 build，但 parse/结构测试不能虚假通过
  未知 Backend；具体行为与阶段 3 的可选 ONNX 配置策略保持一致。

## 5. 按顺序实施

### 5.1 S4-01：扩展共享 tokenizer 的 Pair Encode

在现有 `BertWordPieceTokenizer` 增加：

```cpp
bool EncodePair(const std::string& query,
                const std::string& candidate,
                size_t max_length,
                std::vector<int64_t>* input_ids,
                std::vector<int64_t>* attention_mask,
                std::vector<int64_t>* token_type_ids,
                std::string* diagnostic = nullptr) const;
```

实施要求：

- 复用同一份 `BasicTokenize` 和 `WordPieceTokenize`；
- 不通过字符串拼接 `query + candidate` 模拟 pair；
- 实现第 2.2 节最长优先截断；
- 所有三个输出先按 `max_length` 初始化；
- 任一失败时三个输出全部清空；
- 原 `Encode` 行为和阶段 3 测试必须不变。

先增加 tokenizer 单测：

- 标准 query/candidate 的 ids、mask、types；
- 空 pair；
- query 长、candidate 长、两者同长；
- `max_length == 3` 和 `< 3`；
- CJK、标点、大小写、WordPiece；
- 非法 UTF-8；
- null output；
- 确认 `[SEP]` 边界和 padding type id。

定向验证：

```bash
cmake --build build -j4
./build/edgeflow_test_core_runner \
  '--gtest_filter=OnnxAndEmbeddingModelTest.*:OnnxAndRerankerModelTest.Tokenizer*'
```

### 5.2 S4-02：增加旧 Rerank Engine 兼容适配器

在 `include/core/session_context.h` 的 `detail` 命名空间增加
`LegacyRerankEngineAdapter final : public IRerankModel`，结构参照现有
`LegacyEmbeddingEngineAdapter`。

职责仅限：

1. 将 `QueryCandidatesBatch` 转为 `TraceableItem<IRerankEngine::PairInput>`；
2. 调用旧 `ScoreTraceableBatch`；
3. 转回 `ScoreBatch`；
4. 保证 null、空输入、失败清空、数量与 provenance 一致；
5. 捕获转换和调用边界异常。

在 `ModelManager::GetModel<T>` 增加仅对 `T == IRerankModel` 生效的兼容分支。

边界：

- Adapter 不做 tokenizer、sigmoid、排序或 top-k；
- Adapter 仅服务旧方言，不能注册为 ModelDefinition；
- 新 `bge_reranker` 不经过 Adapter；
- Node 测试应主要使用 Fake `IRerankModel`，不要依赖该过渡层掩盖错误。

测试：

- 旧 Mock NPU Rerank 能被 `GetModel<IRerankModel>` 获取；
- 输入输出转换保持 req/sub；
- 旧 Engine 返回错误或错误数量时输出为空；
- `pipeline_doc_qa_rerank.json` 和 `pipeline_dialogue_audit.json` 仍可 Build/Process。

### 5.3 S4-03：实现 BgeRerankerModel

类声明：

```cpp
class BgeRerankerModel final : public IRerankModel {
 public:
  inline static constexpr char kModelType[] = "bge_reranker";
  inline static constexpr char kCapability[] = "rerank";

  static std::shared_ptr<IModel> Create(const ModelCreateContext& ctx,
                                        std::string* diagnostic);

  const std::string& ModelType() const noexcept override;
  const std::string& Capability() const noexcept override;
  InferenceConcurrency Concurrency() const noexcept override;
  size_t GetMaxBatchSize() const noexcept override;
  int Score(const QueryCandidatesBatch& inputs,
            ScoreBatch* outputs) noexcept override;
};
```

`Create` 的执行顺序：

1. 检查 `backend_session` 非空；
2. `dynamic_pointer_cast<ITensorGraphSession>`；
3. 校验并读取第 2.5 节配置；
4. 校验 Session BatchPolicy；固定 batch 大于 Model 上限时拒绝；
5. 安全解析 tokenizer sidecar，行为必须与阶段 3 一致，拒绝 `../`、同前缀目录和 symlink
   逃逸；优先抽取共用 helper，禁止复制一份不安全实现；
6. 加载 tokenizer；
7. 校验 Session 输入和目标输出 metadata；
8. 创建 Model；任一步失败返回 null 和可定位 diagnostic。

`Score`：

1. 执行第 2.1 节通用边界；
2. 从 Session 取得 `BatchPolicy`；动态 batch 的上限取 Model/Session 最小值；
3. 调用新版 `FixedBatchExecutor::Execute`；
4. callback 只按 `BatchSlice{offset, valid_count, execution_count}` 构造当前批；
5. dummy 样本使用空 query/candidate 编码，但由 Executor 统一剥离；
6. 任一 callback 失败时最终输出为空。

每批执行：

1. 使用 `CreateHostTensor` 创建 2 或 3 个输入 Tensor；
2. 使用 `EncodePair` 填充每一行；
3. 只传 Session 声明的输入；
4. 调用 `session_->Run`；
5. 精确查找 `output_name`；
6. 按第 2.4 节校验 Tensor；
7. decode activation 并拒绝非有限 score；
8. 返回长度为 `execution_count` 的 batch score，由 Executor 删除 dummy 并恢复 provenance。

不得调用或包含：

- `Ort::Session`、`Ort::Value`、`onnxruntime_cxx_api.h`；
- `IRerankEngine`；
- Node、AlgContext、Blackboard；
- 排序和 top-k。

### 5.4 S4-04：迁移 TextRerankNode

修改点：

- `ModelBoundNode<IRerankEngine>` 改为 `ModelBoundNode<IRerankModel>`；
- include 从 `engine_interface.h` 改为 `model_interface.h`；
- 将收集容器直接改为 `QueryCandidatesBatch`；
- 三种输入端口仍统一生成 `QueryCandidatePair{query, candidate}`；
- 调用 `model()->Score(pair_items, &pair_scores)`；
- 成功后要求 score 数量等于 candidate payload 数量；
- 每个 score 的 `(req_id, sub_id)` 必须与对应输入一致，不符时 fail-closed；
- 后续按 req_id 分组、score 降序、top-k 和 `RankedTextBatch` 组装保持原实现；
- NodeDefinition 的端口、constraint、`model_capability = "rerank"`、bind 字段全部不变。

必须保留三种输入组：

1. `[pairs]`；
2. `[queries, candidates]`；
3. `[queries, candidate_texts]`。

Node 单测 Fake 改为 `IRerankModel`，覆盖：

- 三种输入组；
- 多 req_id 分组；
- top-k；
- 空候选；
- Model 错误；
- Model 返回少/多 score；
- provenance 不一致；
- 端口组合约束；
- 输出的 `original_sub_id` 与现有行为一致。

### 5.5 S4-05：注册构建、迁移配置并删除旧 ONNX Rerank Engine

按以下顺序：

1. 把 `bge_reranker_model.cpp` 加入 `FRAMEWORK_SRCS`；
2. 把新测试加入 `EDGEFLOW_TEST_CORE_SRCS`；
3. 注册 `OnnxAndRerankerModelTest` 到 CTest，并使用 tier1/sanitizer-compatible 标签；
4. 完成第 4 节两个配置迁移；
5. 更新旧 Engine/Catalog 断言；
6. 删除 `onnx_rerank_engine.h/.cpp` 及 CMake 引用；
7. 全仓搜索确认不存在生产注册或配置引用。

搜索门禁：

```bash
rg -n 'REGISTER_ENGINE_WITH_DEFINITION\(OnnxRerankEngine|engine_type.*onnx_rerank|OnnxRerankEngine' \
  src include configs CMakeLists.txt cmake tests
```

预期：

- 生产代码和目标配置无结果；
- 测试中只允许出现“确认旧类型不存在”的负向断言或历史文档引用；
- `IRerankEngine` 与 `mock_npu_rerank` 仍存在，留给后续总收口。

### 5.6 S4-06：增加真实 ONNX Rerank fixture

扩展 `scripts/generate_test_onnx_model.py`，在不引入 `onnx`/`numpy` Python 依赖的前提下
继续使用标准库生成：

- `embedding_fixture.onnx`（现有，字节和语义不得意外改变）；
- `rerank_fixture.onnx`（新增）；
- `vocab.txt`（共享）。

Rerank fixture 契约：

| 项 | 值 |
| :--- | :--- |
| inputs | `input_ids`, `attention_mask`, `token_type_ids` |
| input dtype | int64 |
| input shape | `[-1, 32]` |
| output | `logits` |
| output dtype | float32 |
| output shape | `[-1, 1]` |

fixture 必须真实使用三个输入，并让不同 candidate 产生确定性不同 logit。建议使用 Cast、Mul、
Add、MatMul 等基础算子；它只证明真实 ORT 边界和 Model decode，不宣称具备预训练 BGE
语义质量。

CMake 增加：

- fixture 输出和依赖；
- `EDGEFLOW_STAGE4_RERANK_ONNX_FIXTURE` compile definition；
- ONNX 开启构建时生成 fixture，目标测试不得默认 skip。

支持外部 artifact 覆盖：

```text
LLM_EDGEFLOW_TEST_RERANK_ONNX
LLM_EDGEFLOW_TEST_RERANK_VOCAB
```

测试日志/验收文档记录：文件名、SHA-256、I/O 名称、dtype、shape、opset 和生成命令。

### 5.7 S4-07：测试矩阵

新建 `tests/test_onnx_and_reranker_model.cpp`，至少覆盖：

#### A. Create 与 Definition

- ModelRegistry 可见 `bge_reranker`；
- capability/protocol/concurrency/config fields 正确；
- null session、错误协议、缺少 vocab、非法配置、sidecar 逃逸；
- 必需输入/输出名称或 metadata 不符；
- Model 上限小于固定 Session batch；
- Creator 异常不越界。

#### B. Pair encode

- 第 5.1 节全部用例。

#### C. Tensor 与 score

- `[B]`、`[B,1]` 成功；
- sigmoid 和 identity；
- 极大正/负 logit 数值稳定；
- rank、dtype、batch、第二维、bytes、alignment、overflow、null buffer；
- NaN/Inf、缺失 output、Backend Run 错误；
- 第二批失败时第一批结果也回滚。

#### D. Batch

- dynamic：1、满批、跨批，尾批不 padding；
- fixed：1、满批、跨批，执行数量固定；
- dummy 剥离；
- `(req_id, sub_id)` 完整保持；
- Model/Session max 冲突。

#### E. 真实 ORT

- Load metadata；
- 真实 Run；
- 相同 pair 稳定；不同 candidate 得到不同有限 score；
- ModelRuntimeFactory 创建成功；
- Pipeline Build + Process；
- Embedding 和 Rerank 的 registration/backend_type 都是 `onnxruntime`；
- `kEngineLoadFailed` 不能当作 smoke 成功。

#### F. ONNX OFF

- 不注册 `onnxruntime` Backend；
- 不注册 `onnx_rerank` Engine；
- `bge_reranker` Definition 本身仍可被 Catalog/Registry 枚举；
- 真实 ORT 专属测试明确 skip；
- 非 ORT 的 tokenizer、Model fake session、Node 和兼容适配测试仍执行。

## 6. 每个里程碑的命令

### 6.1 格式和定向构建

```bash
./scripts/format.sh
cmake -S . -B build
cmake --build build -j4
./build/edgeflow_test_core_runner \
  '--gtest_filter=OnnxAndRerankerModelTest.*'
./build/edgeflow_test_nodes_runner \
  '--gtest_filter=TextRerankNodeTest.*:CommonNodesTest.TextRerankNode*'
```

### 6.2 配置 validate/plan

```bash
./build/alg_pipeline_tool validate configs/pipeline_cross_rerank.json
./build/alg_pipeline_tool plan configs/pipeline_cross_rerank.json
./build/alg_pipeline_tool validate configs/pipeline_doc_qa_rerank_real.json
./build/alg_pipeline_tool plan configs/pipeline_doc_qa_rerank_real.json
./build/alg_pipeline_tool validate configs/pipeline_doc_qa_rerank.json
./build/alg_pipeline_tool plan configs/pipeline_doc_qa_rerank.json
./build/alg_pipeline_tool validate configs/pipeline_dialogue_audit.json
./build/alg_pipeline_tool plan configs/pipeline_dialogue_audit.json
```

### 6.3 Catalog

```bash
./build/alg_pipeline_tool catalog
```

确认：

- models 包含 `bge_embedding`、`bge_reranker`；
- backends 只有一个 `onnxruntime`；
- engines 不再包含 `onnx_rerank`；
- `TextRerankNode` capability 仍是 `rerank`。

### 6.4 ONNX OFF 独立构建

不要覆盖正常 build：

```bash
cmake -S . -B build-no-onnx-stage4 -G Ninja \
  -DENABLE_ONNXRUNTIME=OFF \
  -DENABLE_LLAMACPP=OFF
cmake --build build-no-onnx-stage4 -j4
ctest --test-dir build-no-onnx-stage4 --output-on-failure
```

### 6.5 Sanitizer 与完整门禁

```bash
LLM_EDGEFLOW_SANITIZERS=undefined ./scripts/run_sanitizers.sh --fast
./scripts/run_all_tests.sh --full
git diff --check
git status --short
```

受支持的 Linux CI 在最终合入 `main` 前补充 ASan/LSan；本地 macOS 26.6 的 Apple Clang 16
ASan 初始化问题不阻塞阶段 4 开发，但必须在最终验收记录中保留门禁状态。

## 7. 推荐提交顺序

每个提交都必须可构建、可测试：

1. `refactor(tokenizer): add deterministic pair encoding`
2. `refactor(rerank): adapt legacy engine to rerank model contract`
3. `feat(rerank): add bge reranker model`
4. `refactor(node): bind text rerank node to rerank model`
5. `feat(test): add real onnx reranker fixture coverage`
6. `refactor(config): migrate onnx rerank pipelines`
7. `refactor(engine): remove legacy onnx rerank engine`
8. `docs(rfc): record stage4 acceptance evidence`

如果实现规模较小，可以合并相邻提交，但禁止把“新增替代实现”和“删除旧实现”放在一个无法
单独验证的大提交中。

## 8. 阶段 4 最终验收清单

### 架构

- [ ] `TextRerankNode` 只依赖 `IRerankModel`；
- [ ] `BgeRerankerModel` 只依赖中性 `ITensorGraphSession`；
- [ ] `OnnxRuntimeBackend` 无 tokenizer、rerank、sigmoid、排序分支；
- [ ] Embedding/Rerank registration 都记录 backend 为 `onnxruntime`；
- [ ] 生产代码只有一份 ONNX Runtime adapter；
- [ ] `OnnxRerankEngine` 源码、注册和构建引用删除；
- [ ] 旧 Mock NPU 配置只通过 ModelManager 兼容适配，不污染 Node。

### 正确性

- [ ] pair packing、截断、mask、token type 有精确单测；
- [ ] 输出 shape/dtype/bytes/alignment/overflow/有限值严格校验；
- [ ] dynamic/fixed/cross-batch、dummy、provenance 全覆盖；
- [ ] Backend/第二批失败时输出完全回滚；
- [ ] 三种 Node 输入组、分组、排序和 top-k 行为不变；
- [ ] 真实 ONNX fixture 非 skip PASS；
- [ ] Pipeline Build/Process smoke 非 skip PASS。

### 配置与质量门禁

- [ ] 两个 `onnx_rerank` 配置完成新方言迁移；
- [ ] 四个 Rerank 业务配置 validate/plan 符合预期；
- [ ] ONNX ON/OFF 均构建通过且无新增 warning；
- [ ] 新测试注册到 CTest；
- [ ] UBSan fast 通过；
- [ ] 完整六阶段门禁 100% 通过；
- [ ] `git diff --check` 通过；
- [ ] 阶段 4 验收证据写回 Markdown；
- [ ] Linux ASan/LSan 仍作为最终合入 `main` 前门禁追踪。

全部勾选后，结论只能写为“阶段 4 实现验收通过，可以进入阶段 5”。在阶段 5–7 和最终
Linux sanitizer/CI 未完成前，RFC-0015 仍保持 `In Implementation`，不能直接合入 `main`。

## 9. 遇到以下情况必须暂停并记录

- 实际 BGE Reranker artifact 的输入名、dtype 或输出 shape 与第 2 节不同；
- 模型需要额外 sidecar 或特殊 pair truncation 策略；
- `score_activation` 不是 identity/sigmoid；
- 为保持旧业务回归必须修改 C ABI、Blackboard Key 或 Node 端口；
- 同一 Backend 无法同时承载 Embedding/Rerank，且原因来自 Backend 的语义分支需求；
- ONNX OFF 时非 ORT 测试也失败；
- 需要新增生产 fallback、固定成功桩或复制第二份 ORT adapter。

这些都可能改变 RFC 契约。先把 artifact metadata、失败命令、diagnostic 和建议变更记录到
阶段 4 验收文档，再决定是否修订 RFC；不要静默偏离本文。

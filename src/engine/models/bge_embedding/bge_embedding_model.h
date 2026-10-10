#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/backend_interface.h"
#include "engine/fixed_batch_executor.h"
#include "engine/model_identity.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"
#include "engine/models/bge_common/bert_wordpiece_tokenizer.h"

namespace llm_edgeflow {

/**
 * @brief 基于 ITensorGraphSession 协议与 WordPiece 分词器的 BGE
 * 文本特征向量提取模型
 *
 * 架构隔离性：
 * - 纯 Model 层实现，继承 IEmbeddingModel；
 * - 只依赖 ITensorGraphSession 中性张量图协议，完全不引用 ONNX Runtime
 * 或第三方头文件；
 * - 加载并验证声明的 WordPiece 词表，执行真实 WordPiece 分词与 padding；
 * - 负责池化 (Pooling: CLS / Mean) 与 L2 归一化；
 * - 使用 FixedBatchExecutor 驱动批次并保持 (req_id, sub_id) 溯源。
 */
class BgeEmbeddingModel final
    : public ModelIdentity<BgeEmbeddingModel, IEmbeddingModel> {
 public:
  inline static constexpr char kImplName[] = "bge_embedding";
  static constexpr InferenceConcurrency kConcurrency =
      InferenceConcurrency::kConcurrent;

  static std::shared_ptr<IModel> Create(const ModelCreateContext& ctx,
                                        std::string* diagnostic);

  BgeEmbeddingModel(std::shared_ptr<ITensorGraphSession> session,
                    BertWordPieceTokenizer tokenizer, size_t max_tokens,
                    std::string pooling, std::string output_name,
                    size_t embedding_dim, bool normalize);

  ~BgeEmbeddingModel() override = default;

  int Embed(const TextBatch& inputs, EmbeddingBatch* outputs,
            std::string* diagnostic = nullptr) noexcept override;

  const BertWordPieceTokenizer& Tokenizer() const noexcept {
    return tokenizer_;
  }
  size_t EmbeddingDim() const noexcept { return embedding_dim_; }
  size_t MaxTokens() const noexcept { return max_tokens_; }
  const std::string& Pooling() const noexcept { return pooling_; }

 private:
  int RawEmbedSlice(const TextBatch& all_inputs, const BatchSlice& slice,
                    std::vector<std::vector<float>>* batch_embeddings,
                    bool normalize_flag, std::string* diagnostic) noexcept;

  std::shared_ptr<ITensorGraphSession> session_;
  BertWordPieceTokenizer tokenizer_;
  size_t max_tokens_ = 512;
  std::string pooling_ = "cls";
  std::string output_name_ = "last_hidden_state";
  size_t embedding_dim_ = 384;
  bool normalize_ = true;
};

}  // namespace llm_edgeflow

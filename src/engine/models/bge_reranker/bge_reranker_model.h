#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/backend_interface.h"
#include "engine/fixed_batch_executor.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"
#include "engine/models/bge_embedding/bert_wordpiece_tokenizer.h"

namespace llm_edgeflow {

/**
 * @brief 基于 ITensorGraphSession 协议与 WordPiece 分词器的 BGE Cross-Encoder
 * 语义精排打分模型
 *
 * 架构隔离性：
 * - 纯 Model 层实现，继承 IRerankModel；
 * - 只依赖 ITensorGraphSession 中性张量图协议，完全不引用 ONNX Runtime
 * 或第三方头文件；
 * - 加载并验证 vocab.txt sidecar，执行 pair 编码与 padding；
 * - 负责 logit 校验与 score 激活 (sigmoid / identity)；
 * - 使用 FixedBatchExecutor 驱动批次并保持 (req_id, sub_id) 溯源。
 */
class BgeRerankerModel final : public IRerankModel {
 public:
  inline static constexpr char kModelType[] = "bge_reranker";
  inline static constexpr char kCapability[] = "rerank";

  static std::shared_ptr<IModel> Create(const ModelCreateContext& ctx,
                                        std::string* diagnostic);

  BgeRerankerModel(std::shared_ptr<ITensorGraphSession> session,
                   BertWordPieceTokenizer tokenizer, size_t max_length,
                   std::string output_name, std::string score_activation,
                   size_t max_batch_size);

  ~BgeRerankerModel() override = default;

  const std::string& ModelType() const noexcept override;
  const std::string& Capability() const noexcept override;
  InferenceConcurrency Concurrency() const noexcept override;
  size_t GetMaxBatchSize() const noexcept override;

  int Score(const QueryCandidatesBatch& inputs,
            ScoreBatch* outputs) noexcept override;

  const BertWordPieceTokenizer& Tokenizer() const noexcept {
    return tokenizer_;
  }
  size_t MaxLength() const noexcept { return max_length_; }
  const std::string& OutputName() const noexcept { return output_name_; }
  const std::string& ScoreActivation() const noexcept {
    return score_activation_;
  }

 private:
  int RawScoreSlice(const QueryCandidatesBatch& all_inputs,
                    const BatchSlice& slice,
                    std::vector<float>* batch_scores) noexcept;

  std::shared_ptr<ITensorGraphSession> session_;
  BertWordPieceTokenizer tokenizer_;
  size_t max_length_ = 512;
  std::string output_name_ = "logits";
  std::string score_activation_ = "sigmoid";
  size_t max_batch_size_ = 4;
};

}  // namespace llm_edgeflow

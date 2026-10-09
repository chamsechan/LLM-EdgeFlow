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
 * @brief 基于 ITensorGraphSession 协议与 WordPiece 分词器的 BGE Cross-Encoder
 * 语义精排打分模型
 *
 * 架构隔离性：
 * - 纯 Model 层实现，继承 IRerankModel；
 * - 只依赖 ITensorGraphSession 中性张量图协议，完全不引用 ONNX Runtime
 * 或第三方头文件；
 * - 加载并验证声明的 WordPiece 词表，执行 pair 编码与 padding；
 * - 负责 logit 校验与 score 激活 (sigmoid / identity)；
 * - 使用 FixedBatchExecutor 驱动批次并保持 (req_id, sub_id) 溯源。
 */
class BgeRerankerModel final
    : public ModelIdentity<BgeRerankerModel, IRerankModel> {
 public:
  inline static constexpr char kImplName[] = "bge_reranker";
  static constexpr InferenceConcurrency kConcurrency =
      InferenceConcurrency::kConcurrent;

  static std::shared_ptr<IModel> Create(const ModelCreateContext& ctx,
                                        std::string* diagnostic);

  BgeRerankerModel(std::shared_ptr<ITensorGraphSession> session,
                   BertWordPieceTokenizer tokenizer, size_t max_tokens,
                   std::string output_name, std::string score_activation);

  ~BgeRerankerModel() override = default;

  int Score(const QueryCandidatesBatch& inputs, ScoreBatch* outputs,
            std::string* diagnostic = nullptr) noexcept override;

  const BertWordPieceTokenizer& Tokenizer() const noexcept {
    return tokenizer_;
  }
  size_t MaxTokens() const noexcept { return max_tokens_; }
  const std::string& OutputName() const noexcept { return output_name_; }
  const std::string& ScoreActivation() const noexcept {
    return score_activation_;
  }

 private:
  int RawScoreSlice(const QueryCandidatesBatch& all_inputs,
                    const BatchSlice& slice, std::vector<float>* batch_scores,
                    std::string* diagnostic) noexcept;

  std::shared_ptr<ITensorGraphSession> session_;
  BertWordPieceTokenizer tokenizer_;
  size_t max_tokens_ = 512;
  std::string output_name_ = "logits";
  std::string score_activation_ = "sigmoid";
};

}  // namespace llm_edgeflow

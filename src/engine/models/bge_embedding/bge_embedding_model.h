#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/backend_interface.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"

namespace alg_framework {

/**
 * @brief 基于 ITensorGraphSession 协议的 BGE 文本特征向量提取模型
 *
 * 架构隔离性：
 * - 纯 Model 层实现，继承 IEmbeddingModel；
 * - 只依赖 ITensorGraphSession 中性张量图协议，完全不引用 ONNX Runtime
 * 或第三方头文件；
 * - 负责分词 (tokenization)、填充 (padding)、池化 (pooling: CLS / Mean) 与 L2
 * 归一化；
 * - 使用 FixedBatchExecutor 驱动批次并保持 (req_id, sub_id) 溯源。
 */
class BgeEmbeddingModel final : public IEmbeddingModel {
 public:
  inline static constexpr char kModelType[] = "bge_embedding";
  inline static constexpr char kCapability[] = "embedding";

  static std::shared_ptr<IModel> Create(const ModelCreateContext& ctx,
                                        std::string* diagnostic);

  BgeEmbeddingModel(std::shared_ptr<ITensorGraphSession> session,
                    size_t max_length, std::string pooling_strategy,
                    bool normalize, size_t max_batch_size,
                    size_t embedding_dim);

  ~BgeEmbeddingModel() override = default;

  const std::string& ModelType() const noexcept override;
  const std::string& Capability() const noexcept override;
  InferenceConcurrency Concurrency() const noexcept override;
  size_t GetMaxBatchSize() const noexcept override;

  int Embed(const TextBatch& inputs, const EmbeddingOptions& options,
            EmbeddingBatch* outputs) noexcept override;

 private:
  int RawEmbedBatch(const std::vector<std::string>& batch_texts,
                    std::vector<std::vector<float>>* batch_embeddings,
                    bool normalize_flag) noexcept;

  std::shared_ptr<ITensorGraphSession> session_;
  size_t max_length_ = 512;
  std::string pooling_strategy_ = "cls";
  bool default_normalize_ = true;
  size_t max_batch_size_ = 4;
  size_t embedding_dim_ = 384;
};

}  // namespace alg_framework

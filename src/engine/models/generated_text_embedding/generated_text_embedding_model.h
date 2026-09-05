#pragma once

#include "engine/backend_interface.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {

// Pools generated-token hidden states. This is a distinct vector space from
// encoder embeddings: callers must evaluate retrieval quality and rebuild
// indexes when changing the model, prompt, generation limit or pooling.
class GeneratedTextEmbeddingModel final : public IEmbeddingModel {
 public:
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic);
  const std::string& ModelType() const noexcept override;
  const std::string& Capability() const noexcept override;
  InferenceConcurrency Concurrency() const noexcept override;
  size_t GetMaxBatchSize() const noexcept override;
  int Embed(const TextBatch& inputs, const EmbeddingOptions& options,
            EmbeddingBatch* outputs) noexcept override;

 private:
  std::shared_ptr<IGeneratedTokenEmbeddingSession> session_;
  std::string prefix_;
  std::string suffix_;
  std::string pooling_;
  int embedding_dim_ = 0;
  int max_tokens_ = 1;
  bool add_bos_ = false;
};

}  // namespace llm_edgeflow

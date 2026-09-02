#pragma once

#include <memory>
#include <string>

#include "engine/model_interface.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {
namespace test {

class TestBusinessEmbeddingModel final : public IEmbeddingModel {
 public:
  inline static constexpr char kModelType[] = "test_business_embedding";
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic);

  TestBusinessEmbeddingModel(size_t embedding_dim, size_t max_batch_size);
  const std::string& ModelType() const noexcept override;
  const std::string& Capability() const noexcept override;
  InferenceConcurrency Concurrency() const noexcept override;
  size_t GetMaxBatchSize() const noexcept override;
  int Embed(const TextBatch& inputs, const EmbeddingOptions& options,
            EmbeddingBatch* outputs) noexcept override;

 private:
  size_t embedding_dim_ = 384;
  size_t max_batch_size_ = 4;
};

class TestBusinessRerankModel final : public IRerankModel {
 public:
  inline static constexpr char kModelType[] = "test_business_rerank";
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic);

  explicit TestBusinessRerankModel(size_t max_batch_size);
  const std::string& ModelType() const noexcept override;
  const std::string& Capability() const noexcept override;
  InferenceConcurrency Concurrency() const noexcept override;
  size_t GetMaxBatchSize() const noexcept override;
  int Score(const QueryCandidatesBatch& inputs,
            ScoreBatch* outputs) noexcept override;

 private:
  size_t max_batch_size_ = 4;
};

class TestBusinessLlmModel final : public ILlmModel {
 public:
  inline static constexpr char kModelType[] = "test_business_llm";
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic);

  explicit TestBusinessLlmModel(size_t max_batch_size);
  const std::string& ModelType() const noexcept override;
  const std::string& Capability() const noexcept override;
  InferenceConcurrency Concurrency() const noexcept override;
  size_t GetMaxBatchSize() const noexcept override;
  int Generate(const TextBatch& prompts, const GenerateOptions& options,
               TextBatch* outputs) noexcept override;

 private:
  size_t max_batch_size_ = 2;
};

class TestBusinessOcrModel final : public IOcrModel {
 public:
  inline static constexpr char kModelType[] = "test_business_ocr";
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic);

  explicit TestBusinessOcrModel(size_t max_batch_size);
  const std::string& ModelType() const noexcept override;
  const std::string& Capability() const noexcept override;
  InferenceConcurrency Concurrency() const noexcept override;
  size_t GetMaxBatchSize() const noexcept override;
  int Recognize(const ImageRefBatch& images,
                OcrDocumentBatch* outputs) noexcept override;

 private:
  size_t max_batch_size_ = 2;
};

class TestBusinessAsrModel final : public IAsrModel {
 public:
  inline static constexpr char kModelType[] = "test_business_asr";
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic);

  explicit TestBusinessAsrModel(size_t max_batch_size);
  const std::string& ModelType() const noexcept override;
  const std::string& Capability() const noexcept override;
  InferenceConcurrency Concurrency() const noexcept override;
  size_t GetMaxBatchSize() const noexcept override;
  int Transcribe(const AudioPcmBatch& audio,
                 TextBatch* outputs) noexcept override;

 private:
  size_t max_batch_size_ = 2;
};

}  // namespace test
}  // namespace llm_edgeflow

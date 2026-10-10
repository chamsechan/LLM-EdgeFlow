#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "engine/backend_interface.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {
namespace test {

class TestBizEmbeddingModel final : public IEmbeddingModel {
 public:
  inline static constexpr char kImplName[] = "test_biz_embedding";
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic);

  TestBizEmbeddingModel(std::shared_ptr<IBackendSession> session,
                        size_t embedding_dim);
  const std::string& ImplName() const noexcept override;
  const std::string& ModelType() const noexcept override;
  InferenceConcurrency Concurrency() const noexcept override;
  int Embed(const TextBatch& inputs, EmbeddingBatch* outputs,
            std::string* diagnostic = nullptr) noexcept override;

 private:
  size_t embedding_dim_ = 384;
  std::shared_ptr<IBackendSession> session_;
};

class TestBizRerankModel final : public IRerankModel {
 public:
  inline static constexpr char kImplName[] = "test_biz_rerank";
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic);

  explicit TestBizRerankModel(std::shared_ptr<IBackendSession> session);
  const std::string& ImplName() const noexcept override;
  const std::string& ModelType() const noexcept override;
  InferenceConcurrency Concurrency() const noexcept override;
  int Score(const QueryCandidatesBatch& inputs, ScoreBatch* outputs,
            std::string* diagnostic = nullptr) noexcept override;

 private:
  std::shared_ptr<IBackendSession> session_;
};

class TestBizLlmModel final : public ILlmModel {
 public:
  bool SupportsRandomSeed() const noexcept override { return true; }
  inline static constexpr char kImplName[] = "test_biz_llm";
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic);

  explicit TestBizLlmModel(std::shared_ptr<IBackendSession> session);
  const std::string& ImplName() const noexcept override;
  const std::string& ModelType() const noexcept override;
  InferenceConcurrency Concurrency() const noexcept override;
  int Generate(const TextBatch& prompts, const GenerateOptions& options,
               TextBatch* outputs,
               std::string* diagnostic = nullptr) noexcept override;

 private:
  std::shared_ptr<IBackendSession> session_;
};

class TestBizOcrModel final : public IOcrModel {
 public:
  inline static constexpr char kImplName[] = "test_biz_ocr";
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic);

  explicit TestBizOcrModel(std::shared_ptr<IBackendSession> session);
  const std::string& ImplName() const noexcept override;
  const std::string& ModelType() const noexcept override;
  InferenceConcurrency Concurrency() const noexcept override;
  int Recognize(const ImageRefBatch& images, OcrDocumentBatch* outputs,
                std::string* diagnostic = nullptr) noexcept override;

 private:
  std::shared_ptr<IBackendSession> session_;
};

class TestBizAsrModel final : public IAsrModel {
 public:
  inline static constexpr char kImplName[] = "test_biz_asr";
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic);

  explicit TestBizAsrModel(std::shared_ptr<IBackendSession> session);
  const std::string& ImplName() const noexcept override;
  const std::string& ModelType() const noexcept override;
  InferenceConcurrency Concurrency() const noexcept override;
  bool SupportsLanguage(std::string_view) const noexcept override {
    return true;
  }
  int Transcribe(const AudioPcmBatch& audio, const TranscribeOptions& options,
                 TextBatch* outputs,
                 std::string* diagnostic = nullptr) noexcept override;

 private:
  std::shared_ptr<IBackendSession> session_;
};

}  // namespace test
}  // namespace llm_edgeflow

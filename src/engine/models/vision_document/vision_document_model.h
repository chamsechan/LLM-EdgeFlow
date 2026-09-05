#pragma once

#include "engine/backend_interface.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {

class VisionDocumentModel final : public IOcrModel {
 public:
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic);
  const std::string& ModelType() const noexcept override;
  const std::string& Capability() const noexcept override;
  InferenceConcurrency Concurrency() const noexcept override;
  size_t GetMaxBatchSize() const noexcept override;
  int Recognize(const ImageRefBatch& images,
                OcrDocumentBatch* outputs) noexcept override;

 private:
  std::shared_ptr<IImageTextGenerationSession> session_;
  std::string prompt_;
  int patch_size_ = 16;
  size_t max_pixels_ = 4194304;
  GenerateOptions options_;
};

}  // namespace llm_edgeflow

#pragma once

#include "engine/backend_interface.h"
#include "engine/model_identity.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {

class VisionDocumentModel final
    : public ModelIdentity<VisionDocumentModel, IOcrModel> {
 public:
  inline static constexpr char kImplName[] = "vision_document";
  static constexpr InferenceConcurrency kConcurrency =
      InferenceConcurrency::kConcurrent;
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic);
  int Recognize(const ImageFrameBatch& images, OcrDocumentBatch* outputs,
                std::string* diagnostic = nullptr) noexcept override;

 private:
  std::shared_ptr<IImageTextGenerationSession> session_;
  std::string prompt_;
  int patch_size_ = 16;
  size_t max_pixels_ = 4194304;
  GenerateOptions options_;
};

}  // namespace llm_edgeflow

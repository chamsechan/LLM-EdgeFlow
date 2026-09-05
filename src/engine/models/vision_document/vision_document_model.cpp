#include "engine/models/vision_document/vision_document_model.h"

#include "company_alg_log.h"
#include "engine/fixed_batch_executor.h"
#include "engine/models/vision_document/image_decode.h"

namespace llm_edgeflow {
namespace {
constexpr char kPrompt[] =
    "Read all text visible in this image. Return only the transcribed text.";
}

std::shared_ptr<IModel> VisionDocumentModel::Create(
    const ModelCreateContext& context, std::string* diagnostic) {
  try {
    auto session = std::dynamic_pointer_cast<IImageTextGenerationSession>(
        context.backend_session);
    if (!session ||
        session->Protocol() != ExecutionProtocol::kImageTextGeneration ||
        session->GetBatchPolicy().max_batch_size != 1 ||
        session->GetBatchPolicy().fixed_batch_size != 0) {
      throw std::runtime_error(
          "vision_document requires a single-image generation session");
    }
    auto model = std::make_shared<VisionDocumentModel>();
    model->session_ = std::move(session);
    model->prompt_ = context.model_config.value("prompt", std::string(kPrompt));
    model->patch_size_ = context.model_config.value("patch_size", 16);
    const int max_pixels = context.model_config.value("max_pixels", 4194304);
    model->options_.max_tokens = context.model_config.value("max_tokens", 512);
    if (model->prompt_.empty() ||
        model->prompt_.find('\0') != std::string::npos ||
        model->patch_size_ < 1 || model->patch_size_ > 256 || max_pixels < 1 ||
        max_pixels > 16777216 || model->options_.max_tokens < 1 ||
        model->options_.max_tokens > 4096) {
      throw std::runtime_error("Invalid vision_document configuration");
    }
    model->max_pixels_ = static_cast<size_t>(max_pixels);
    model->options_.temperature = 0.0f;
    model->options_.top_p = 1.0f;
    return model;
  } catch (const std::exception& e) {
    inference_detail::SetDiagnostic(diagnostic, e.what());
    return nullptr;
  } catch (...) {
    inference_detail::SetDiagnostic(diagnostic,
                                    "Unknown vision_document creation error");
    return nullptr;
  }
}

const std::string& VisionDocumentModel::ModelType() const noexcept {
  static const std::string value = "vision_document";
  return value;
}
const std::string& VisionDocumentModel::Capability() const noexcept {
  static const std::string value = "ocr";
  return value;
}
InferenceConcurrency VisionDocumentModel::Concurrency() const noexcept {
  return InferenceConcurrency::kConcurrent;
}
size_t VisionDocumentModel::GetMaxBatchSize() const noexcept { return 1; }

int VisionDocumentModel::Recognize(const ImageRefBatch& images,
                                   OcrDocumentBatch* outputs) noexcept {
  if (!outputs) return -1;
  outputs->clear();
  if (!session_) return -1;
  return FixedBatchExecutor::Execute<std::string, OcrDocumentItem>(
      images, session_->GetBatchPolicy(),
      [this, &images](const BatchSlice& slice,
                      std::vector<OcrDocumentItem>* batch) {
        ImageTextInput request;
        std::string diagnostic;
        if (!DecodeDocumentImage(images[slice.offset].data, patch_size_,
                                 max_pixels_, &request, &diagnostic)) {
          ALG_LOG_ERROR("[VisionDocumentModel] %s\n", diagnostic.c_str());
          return -1;
        }
        request.prompt = prompt_;
        OcrDocumentItem document;
        const int result = session_->Generate(
            request, options_, &document.combined_text, &diagnostic);
        if (result != 0 || document.combined_text.empty()) {
          ALG_LOG_ERROR("[VisionDocumentModel] Generation failed: %s\n",
                        diagnostic.c_str());
          return -1;
        }
        // Generative recognition has no measured boxes or confidence scores.
        batch->push_back(std::move(document));
        return 0;
      },
      outputs);
}

static const ModelDefinition kVisionDocumentDefinition = [] {
  ModelDefinition definition;
  definition.model_type = "vision_document";
  definition.capability = "ocr";
  definition.description =
      "Image-to-text document recognition; text only, no detected boxes";
  definition.required_protocol = ExecutionProtocol::kImageTextGeneration;
  definition.concurrency = InferenceConcurrency::kConcurrent;
  definition.config_fields = {
      {"prompt", ConfigValueKind::kString, false, kPrompt},
      {"patch_size", ConfigValueKind::kInteger, false, 16, 1.0, 256.0},
      {"max_pixels", ConfigValueKind::kInteger, false, 4194304, 1.0,
       16777216.0},
      {"max_tokens", ConfigValueKind::kInteger, false, 512, 1.0, 4096.0}};
  return definition;
}();
REGISTER_MODEL_WITH_DEFINITION(VisionDocumentModel, kVisionDocumentDefinition);

}  // namespace llm_edgeflow

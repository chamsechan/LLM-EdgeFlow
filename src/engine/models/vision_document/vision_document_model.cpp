#include "engine/models/vision_document/vision_document_model.h"

#include "contracts/diagnostic.h"
#include "edgeflow/log.h"
#include "engine/fixed_batch_executor.h"
#include "engine/models/vision_document/image_decode.h"

namespace llm_edgeflow {

static const ModelDefinition& VisionDocumentDefinition();

namespace {
constexpr char kPrompt[] =
    "Read all text visible in this image. Return only the transcribed text.";

bool ValidateVisionConfig(const nlohmann::json& config,
                          std::string* diagnostic) {
  const auto prompt = ConfigValueOrDefault<std::string>(
      config, VisionDocumentDefinition().config_fields, "prompt");
  if (prompt.empty() || prompt.find('\0') != std::string::npos) {
    SetDiagnosticNoexcept(
        diagnostic, "Field 'prompt' must be non-empty and contain no NUL");
    return false;
  }
  return true;
}
}  // namespace

std::shared_ptr<IModel> VisionDocumentModel::Create(
    const ModelCreateContext& context, std::string* diagnostic) {
  try {
    if (!ValidateVisionConfig(context.model_config, diagnostic)) return nullptr;
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
    model->prompt_ = ConfigValueOrDefault<std::string>(
        context.model_config, VisionDocumentDefinition().config_fields,
        "prompt");
    model->patch_size_ = ConfigValueOrDefault<int>(
        context.model_config, VisionDocumentDefinition().config_fields,
        "patch_size");
    const int max_pixels = ConfigValueOrDefault<int>(
        context.model_config, VisionDocumentDefinition().config_fields,
        "max_pixels");
    model->options_.max_tokens = ConfigValueOrDefault<int>(
        context.model_config, VisionDocumentDefinition().config_fields,
        "max_tokens");
    if (model->patch_size_ < 1 || model->patch_size_ > 256 || max_pixels < 1 ||
        max_pixels > 16777216 || model->options_.max_tokens < 1 ||
        model->options_.max_tokens > 4096) {
      throw std::runtime_error("Invalid vision_document configuration");
    }
    model->max_pixels_ = static_cast<size_t>(max_pixels);
    model->options_.temperature = 0.0f;
    model->options_.top_p = 1.0f;
    return model;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(diagnostic, e.what());
    return nullptr;
  } catch (...) {
    SetDiagnosticNoexcept(diagnostic, "Unknown vision_document creation error");
    return nullptr;
  }
}

int VisionDocumentModel::Recognize(const ImageRefBatch& images,
                                   OcrDocumentBatch* outputs,
                                   std::string* diagnostic) noexcept {
  if (diagnostic) diagnostic->clear();
  if (!outputs) {
    SetDiagnosticNoexcept(diagnostic, "Model output pointer is null");
    return -1;
  }
  outputs->clear();
  if (!session_) {
    SetDiagnosticNoexcept(diagnostic, "Model session is null");
    return -1;
  }
  return FixedBatchExecutor::ExecuteItems<std::string, OcrDocumentItem>(
      images, session_->GetBatchPolicy(),
      [this, diagnostic](const TraceableItem<std::string>& image,
                         OcrDocumentItem* output) {
        ImageTextInput request;
        std::string reason;
        if (!DecodeDocumentImage(image.data, patch_size_, max_pixels_, &request,
                                 &reason)) {
          ALG_LOG_ERROR("[VisionDocumentModel] %s\n", reason.c_str());
          SetDiagnosticNoexcept(diagnostic, reason);
          return -1;
        }
        request.prompt = prompt_;
        OcrDocumentItem document;
        const int result = session_->Generate(request, options_,
                                              &document.combined_text, &reason);
        if (result != 0 || document.combined_text.empty()) {
          ALG_LOG_ERROR("[VisionDocumentModel] Generation failed: %s\n",
                        reason.c_str());
          SetDiagnosticNoexcept(
              diagnostic,
              reason.empty() ? "Image generation returned empty text" : reason);
          return -1;
        }
        // 生成式识别没有实测的框或置信度。
        *output = std::move(document);
        return 0;
      },
      outputs, diagnostic);
}

static const ModelDefinition& VisionDocumentDefinition() {
  static const ModelDefinition definition = [] {
    auto definition = MakeModelDefinition<VisionDocumentModel>();
    definition.description =
        "Image-to-text document recognition; text only, no detected boxes";
    definition.required_protocol = ExecutionProtocol::kImageTextGeneration;
    definition.validate_config = ValidateVisionConfig;
    definition.config_fields = {
        {"prompt",
         ConfigValueKind::kString,
         false,
         kPrompt,
         std::nullopt,
         std::nullopt,
         {},
         "图像转文本的识别指令；须为非空文本，识别输出作为文档全文。"},
        {"patch_size",
         ConfigValueKind::kInteger,
         false,
         16,
         1.0,
         256.0,
         {},
         "图像块边长，单位为像素；解码后宽高补齐到其整数倍，须符合图像模型约定"
         "。"},
        {"max_pixels",
         ConfigValueKind::kInteger,
         false,
         4194304,
         1.0,
         16777216.0,
         {},
         "允许的图像总像素数上限，同时检查原图与按 patch_size 补齐后的图像。"},
        {"max_tokens",
         ConfigValueKind::kInteger,
         false,
         512,
         1.0,
         4096.0,
         {},
         "每张图像识别时最多生成的文本 token 数；该路径使用贪心生成。"}};
    return definition;
  }();
  return definition;
}
REGISTER_MODEL_WITH_DEFINITION(VisionDocumentModel, VisionDocumentDefinition());

}  // namespace llm_edgeflow

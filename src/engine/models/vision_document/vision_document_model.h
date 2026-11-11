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
  /**
   * @brief
   * 将每帧图像补齐为模型输入后，以贪心生成获取文档全文；不生成检测框或置信度。
   * 空识别文本视为失败；任一图像处理失败时清空整批输出，成功时保留来源编号。
   */
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

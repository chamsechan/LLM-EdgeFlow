#include <iostream>
#include <string>
#include <vector>

#include "core/alg_context.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"

namespace alg_framework {

/**
 * @brief OCR 推理识别算子
 */
class OcrInferNode : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    std::string bind_model_id = config.value("bind_model", "ocr_model_v1");
    ocr_engine_ =
        session_ctx->GetModelManager().GetModel<IOcrEngine>(bind_model_id);
    if (!ocr_engine_) {
      std::cerr << "[OcrInferNode] Failed to get IOcrEngine model: "
                << bind_model_id << std::endl;
      return false;
    }
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* image_items = req_ctx->Get<std::vector<TraceableItem<std::string>>>(
        "traceable_image_items");
    if (!image_items) {
      req_ctx->SetError(-5201, "OcrInferNode: Missing traceable_image_items");
      return -5201;
    }

    std::vector<TraceableItem<std::vector<IOcrEngine::OcrBoxItem>>>
        detected_boxes;
    int ret = ocr_engine_->InferTraceableBatch(*image_items, &detected_boxes);
    if (ret != 0) {
      req_ctx->SetError(ret, "OcrInferNode: OCR inference failed");
      return ret;
    }

    // 格式化 OCR 文字拼接为上下文
    auto* raw_queries = req_ctx->Get<std::vector<std::string>>("raw_queries");
    auto* req_ids = req_ctx->Get<std::vector<uint64_t>>("raw_request_ids");
    std::vector<TraceableItem<std::string>> llm_prompts;
    std::vector<int> box_counts;

    for (size_t i = 0; i < detected_boxes.size(); ++i) {
      std::string ocr_text_summary;
      for (const auto& box : detected_boxes[i].data) {
        ocr_text_summary += box.text + "\n";
      }
      box_counts.push_back(static_cast<int>(detected_boxes[i].data.size()));

      std::string prompt = "【图片识别OCR文本】:\n" + ocr_text_summary +
                           "\n【用户提取指令】: " + (*raw_queries)[i] +
                           "\n请以标准JSON格式返回发票结构化字段:";
      llm_prompts.emplace_back((*req_ids)[i], 0, prompt);
    }

    req_ctx->Set("ocr_box_counts", std::move(box_counts));
    req_ctx->Set("llm_input_prompts", std::move(llm_prompts));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "OcrInferNode";
    return name;
  }

 private:
  std::shared_ptr<IOcrEngine> ocr_engine_;
};

REGISTER_NODE(OcrInferNode);

}  // namespace alg_framework

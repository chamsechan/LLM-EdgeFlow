#include <string>
#include <vector>

#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "engine/engine_interface.h"
#include "nodes/model_bound_node.h"

namespace alg_framework {

/**
 * @brief OCR 视觉文档检测与文字识别通用算子 (OcrDetectNode, 调用绑定的
 * IOcrEngine)
 */
class OcrDetectNode final : public ModelBoundNode<IOcrEngine> {
 public:
  inline static constexpr char kNodeType[] = "OcrDetectNode";

  OcrDetectNode()
      : ModelBoundNode<IOcrEngine>(kNodeType),
        in_images_("images"),
        out_doc_("document"),
        out_text_("text") {}

 protected:
  bool InitModelNode(const NodeInitContext& init_ctx,
                     const nlohmann::json& /*config*/,
                     SessionContext& /*session_ctx*/) override {
    BindPort(init_ctx, in_images_);
    BindPort(init_ctx, out_doc_);
    BindPort(init_ctx, out_text_);
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* image_items =
        in_images_.Require(req_ctx, -7101, "OcrDetectNode image input");
    if (!image_items) {
      return -7101;
    }

    if (image_items->empty()) {
      out_doc_.Set(req_ctx, OcrDocumentBatch{});
      out_text_.Set(req_ctx, TextBatch{});
      return 0;
    }

    std::vector<TraceableItem<std::vector<IOcrEngine::OcrBoxItem>>> raw_boxes;
    ALG_LOG_DEBUG(
        "[OcrDetectNode] Executing OCR text detection for %zu image files...\n",
        image_items->size());

    int ret = engine()->InferTraceableBatch(*image_items, &raw_boxes);
    if (ret != 0) {
      return Fail(req_ctx, ret, "OcrDetectNode: OCR inference failed");
    }

    OcrDocumentBatch doc_batch;
    TextBatch text_batch;
    doc_batch.reserve(raw_boxes.size());
    text_batch.reserve(raw_boxes.size());

    for (const auto& item : raw_boxes) {
      OcrDocumentItem doc_item;
      doc_item.boxes.reserve(item.data.size());
      std::string combined;

      for (size_t b = 0; b < item.data.size(); ++b) {
        const auto& box = item.data[b];
        doc_item.boxes.push_back(
            {box.x, box.y, box.width, box.height, box.text, box.confidence});
        if (b > 0) combined += "\n";
        combined += box.text;
      }
      doc_item.combined_text = combined;

      doc_batch.emplace_back(item.req_id, item.sub_id, std::move(doc_item));
      text_batch.emplace_back(item.req_id, item.sub_id, std::move(combined));
    }

    out_doc_.Set(req_ctx, std::move(doc_batch));
    out_text_.Set(req_ctx, std::move(text_batch));
    return 0;
  }

 private:
  BoundInput<ImageRefBatch> in_images_;
  BoundOutput<OcrDocumentBatch> out_doc_;
  BoundOutput<TextBatch> out_text_;
};

NodeDefinition MakeOcrDetectNodeDefinition() {
  NodeDefinition def;
  def.node_type = OcrDetectNode::kNodeType;
  def.category = "common";
  def.description = "OCR visual document detection and text recognition node";
  def.inputs = {RequiredInputPort(
      "images", BlackboardKey<ImageRefBatch>{"", "ImageRefBatch"}, "1:1",
      "preserve", "request")};
  def.outputs = {
      OutputPort("document",
                 BlackboardKey<OcrDocumentBatch>{"", "OcrDocumentBatch"}, false,
                 "1:1", "preserve", "request"),
      OutputPort("text", BlackboardKey<TextBatch>{"", "TextBatch"},
                 /*allow_override=*/true, "1:1", "preserve", "request")};
  def.config_fields = {ConfigFieldDefinition{
      "bind_model", ConfigValueKind::kString, false, "ocr_model_v1"}};
  def.model_capability = "ocr";
  def.model_config_field = "bind_model";
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(OcrDetectNode, MakeOcrDetectNodeDefinition());

}  // namespace alg_framework
#include "company_alg_log.h"

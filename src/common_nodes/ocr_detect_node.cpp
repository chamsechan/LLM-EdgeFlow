#include <string>
#include <vector>

#include "company_alg_log.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "engine/model_interface.h"
#include "nodes/model_bound_node.h"
#include "nodes/node_error_codes.h"

namespace alg_framework {

/**
 * @brief OCR 视觉文档检测与文字识别通用算子 (OcrDetectNode, 调用绑定的
 * IOcrModel)
 */
class OcrDetectNode final : public ModelBoundNode<IOcrModel> {
 public:
  inline static constexpr char kNodeType[] = "OcrDetectNode";

  OcrDetectNode()
      : ModelBoundNode<IOcrModel>(kNodeType),
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
        in_images_.Require(req_ctx, node_error::ocr_detect::kMissingInput,
                           "OcrDetectNode image input");
    if (!image_items) {
      return node_error::ocr_detect::kMissingInput;
    }

    if (image_items->empty()) {
      out_doc_.Set(req_ctx, OcrDocumentBatch{});
      out_text_.Set(req_ctx, TextBatch{});
      return 0;
    }

    OcrDocumentBatch doc_batch;
    ALG_LOG_DEBUG(
        "[OcrDetectNode] Executing OCR text detection for %zu image files...\n",
        image_items->size());

    int ret = model()->Recognize(*image_items, &doc_batch);
    if (ret != 0) {
      return Fail(req_ctx, ret, "OcrDetectNode: OCR inference failed");
    }

    if (doc_batch.size() != image_items->size()) {
      return Fail(req_ctx, node_error::ocr_detect::kOutputCountMismatch,
                  "OcrDetectNode: document count mismatch");
    }

    TextBatch text_batch;
    text_batch.reserve(doc_batch.size());
    for (size_t i = 0; i < doc_batch.size(); ++i) {
      const auto& document = doc_batch[i];
      const auto& image = (*image_items)[i];
      if (document.req_id != image.req_id || document.sub_id != image.sub_id) {
        return Fail(req_ctx, node_error::ocr_detect::kOutputProvenanceMismatch,
                    "OcrDetectNode: document provenance mismatch");
      }
      text_batch.emplace_back(document.req_id, document.sub_id,
                              document.data.combined_text);
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
                 /*allow_override=*/false, "1:1", "preserve", "request")};
  def.config_fields = {ConfigFieldDefinition{
      "bind_model", ConfigValueKind::kString, false, "ocr_model_v1"}};
  def.model_capability = "ocr";
  def.model_config_field = "bind_model";
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(OcrDetectNode, MakeOcrDetectNodeDefinition());

}  // namespace alg_framework

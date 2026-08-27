#include <iostream>
#include <string>
#include <vector>

#include "biz/ocr_doc_qa/ocr_doc_qa_contract.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"
#include "nodes/model_bound_node.h"

namespace alg_framework {

/**
 * @brief OCR 推理识别算子
 */
class OcrInferNode final : public ModelBoundNode<IOcrEngine> {
 public:
  inline static constexpr char kNodeType[] = "OcrInferNode";

  OcrInferNode() : ModelBoundNode<IOcrEngine>(kNodeType, "ocr_model_v1") {}

 protected:
  int ProcessNode(AlgContext& req_ctx) override {
    const auto* image_items = Require(req_ctx, kTraceableImageItems, -5201);
    const auto* raw_queries = Require(req_ctx, kRawQueries, -5201);
    const auto* req_ids = Require(req_ctx, kRawRequestIds, -5201);

    if (!image_items || !raw_queries || !req_ids) {
      return -5201;
    }

    std::vector<TraceableItem<std::vector<IOcrEngine::OcrBoxItem>>>
        detected_boxes;
    int ret = engine()->InferTraceableBatch(*image_items, &detected_boxes);
    if (ret != 0) {
      return Fail(req_ctx, ret, "OcrInferNode: OCR inference failed");
    }

    // 格式化 OCR 文字拼接为上下文
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

    Publish(req_ctx, kOcrBoxCounts, std::move(box_counts));
    Publish(req_ctx, kLlmInputPrompts, std::move(llm_prompts));
    return 0;
  }
};

NodeDefinition MakeOcrInferNodeDefinition() {
  NodeDefinition def;
  def.node_type = OcrInferNode::kNodeType;
  def.category = "biz";
  def.description = "OCR text detection and prompt generation inference node";
  def.inputs = {RequiredInput(kTraceableImageItems), RequiredInput(kRawQueries),
                RequiredInput(kRawRequestIds)};
  def.outputs = {Output(kOcrBoxCounts), Output(kLlmInputPrompts)};
  def.config_fields = {ConfigFieldDefinition{
      "bind_model", ConfigValueKind::kString, false, "ocr_model_v1"}};
  def.model_capability = "ocr";
  def.model_config_field = "bind_model";
  def.biz_names = {kOcrDocQaBizName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(OcrInferNode, MakeOcrInferNodeDefinition());

}  // namespace alg_framework

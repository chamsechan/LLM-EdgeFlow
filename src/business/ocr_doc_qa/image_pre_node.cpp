#include <iostream>
#include <string>
#include <vector>

#include "business/ocr_doc_qa/ocr_doc_qa_contract.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "nodes/node_support.h"

namespace alg_framework {

/**
 * @brief 图片与发票前处理算子
 */
class ImagePreNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "ImagePreNode";

  ImagePreNode() : NodeBase(kNodeType) {}

 protected:
  int ProcessNode(AlgContext& req_ctx) override {
    const auto* raw_images = Require(req_ctx, kRawImagePaths, -5101);
    const auto* raw_queries = Require(req_ctx, kRawQueries, -5101);
    const auto* req_ids = Require(req_ctx, kRawRequestIds, -5101);

    if (!raw_images || !raw_queries || !req_ids) {
      return -5101;
    }

    std::vector<TraceableItem<std::string>> image_items;
    for (size_t i = 0; i < raw_images->size(); ++i) {
      image_items.emplace_back((*req_ids)[i], 0, (*raw_images)[i]);
    }

    Publish(req_ctx, kTraceableImageItems, std::move(image_items));
    return 0;
  }
};

NodeDefinition MakeImagePreNodeDefinition() {
  NodeDefinition def;
  def.node_type = ImagePreNode::kNodeType;
  def.category = "business";
  def.description = "OCR image pre-processing node";
  def.inputs = {RequiredInput(kRawImagePaths), RequiredInput(kRawQueries),
                RequiredInput(kRawRequestIds)};
  def.outputs = {Output(kTraceableImageItems)};
  def.business_names = {kOcrDocQaBusinessName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(ImagePreNode, MakeImagePreNodeDefinition());

}  // namespace alg_framework

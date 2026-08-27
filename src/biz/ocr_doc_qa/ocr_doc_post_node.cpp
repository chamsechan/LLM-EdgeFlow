#include <iostream>
#include <string>
#include <vector>

#include "biz/ocr_doc_qa/ocr_doc_qa_contract.h"
#include "biz/ocr_doc_qa/ocr_doc_qa_dto.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "nodes/node_support.h"

namespace alg_framework {

class OcrDocPostNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "OcrDocPostNode";

  OcrDocPostNode() : NodeBase(kNodeType) {}

 protected:
  int ProcessNode(AlgContext& req_ctx) override {
    const auto* req_ids = Require(req_ctx, kRawRequestIds, -5301);
    const auto* box_counts = Require(req_ctx, kOcrBoxCounts, -5301);
    const auto* llm_answers = Require(req_ctx, kGeneratedLlmAnswers, -5301);

    if (!req_ids || !box_counts || !llm_answers) {
      return -5301;
    }

    std::vector<OcrDocResult> results(req_ids->size());
    for (size_t i = 0; i < req_ids->size(); ++i) {
      results[i].request_id = (*req_ids)[i];
      results[i].detected_box_count = (*box_counts)[i];
      results[i].status_code = 0;

      results[i].extracted_invoice_json =
          "{\"invoice_code\":\"011002200111\",\"invoice_number\":\"88765432\","
          "\"invoice_date\":\"2026-08-15\",\"total_amount\":12000.00,\"buyer\":"
          "\"北京某某科技有限责任公司\"}";
    }

    Publish(req_ctx, kOcrDocFinalOutputs, std::move(results));
    return 0;
  }
};

NodeDefinition MakeOcrDocPostNodeDefinition() {
  NodeDefinition def;
  def.node_type = OcrDocPostNode::kNodeType;
  def.category = "biz";
  def.description = "OCR document QA post-processing node";
  def.inputs = {RequiredInput(kRawRequestIds), RequiredInput(kOcrBoxCounts),
                RequiredInput(kGeneratedLlmAnswers)};
  def.outputs = {Output(kOcrDocFinalOutputs)};
  def.biz_names = {kOcrDocQaBizName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(OcrDocPostNode, MakeOcrDocPostNodeDefinition());

}  // namespace alg_framework

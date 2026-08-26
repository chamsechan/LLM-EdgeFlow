#include <iostream>
#include <string>
#include <vector>

#include "business/doc_qa/doc_qa_contract.h"
#include "business/doc_qa/doc_qa_dto.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "nodes/node_support.h"

namespace alg_framework {

/**
 * @brief 文档问答后处理与多样本对齐算子
 */
class DocQaPostNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "DocQaPostNode";

  DocQaPostNode() : NodeBase(kNodeType) {}

 protected:
  int ProcessNode(AlgContext& req_ctx) override {
    const auto* raw_req_ids = Require(req_ctx, kRawRequestIds, -4401);
    const auto* intents = Require(req_ctx, kRecognizedIntents, -4401);
    const auto* answers = Require(req_ctx, kGeneratedLlmAnswers, -4401);
    const auto* confidences = req_ctx.Get(kIntentConfidences);
    const auto* chunk_counts = req_ctx.Get(kChunkCountsPerReq);

    if (!raw_req_ids || !intents || !answers) {
      return -4401;
    }

    size_t batch_size = raw_req_ids->size();
    std::vector<DocQaResult> final_outputs(batch_size);

    // 默认初始化
    for (size_t i = 0; i < batch_size; ++i) {
      final_outputs[i].request_id = (*raw_req_ids)[i];
      final_outputs[i].status_code = 0;
      final_outputs[i].chunk_count =
          (chunk_counts && i < chunk_counts->size()) ? (*chunk_counts)[i] : 0;
      final_outputs[i].confidence =
          (confidences && i < confidences->size()) ? (*confidences)[i] : 0.0f;
      final_outputs[i].intent_name = (*intents)[i];
    }

    // 根据 req_id 填充 LLM 生成回答
    for (const auto& ans_item : *answers) {
      uint32_t r_id = ans_item.req_id;
      if (r_id < batch_size) {
        final_outputs[r_id].answer_text = ans_item.data;
      }
    }

    std::cout << "[DocQaPostNode] Successfully aggregated and aligned "
              << batch_size << " output results." << std::endl;

    Publish(req_ctx, kFinalDocOutputs, std::move(final_outputs));
    return 0;
  }
};

NodeDefinition MakeDocQaPostNodeDefinition() {
  NodeDefinition def;
  def.node_type = DocQaPostNode::kNodeType;
  def.category = "business";
  def.description = "Document QA aggregation and post-processing node";
  def.inputs = {
      RequiredInput(kRawRequestIds), RequiredInput(kRecognizedIntents),
      OptionalInput(kIntentConfidences), OptionalInput(kChunkCountsPerReq),
      RequiredInput(kGeneratedLlmAnswers)};
  def.outputs = {Output(kFinalDocOutputs)};
  def.business_names = {kDocQaBusinessName, kDocQaOnnxBusinessName,
                        kDocQaRerankBusinessName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(DocQaPostNode, MakeDocQaPostNodeDefinition());

}  // namespace alg_framework

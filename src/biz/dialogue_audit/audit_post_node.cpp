#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "biz/dialogue_audit/dialogue_audit_contract.h"
#include "biz/dialogue_audit/dialogue_audit_dto.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "nodes/node_support.h"

namespace alg_framework {

/**
 * @brief 风控审核后处理与结果打包算子 (Node 6: 组装输出领域 DTO)
 */
class AuditPostNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "AuditPostNode";

  AuditPostNode() : NodeBase(kNodeType) {}

 protected:
  int ProcessNode(AlgContext& req_ctx) override {
    const auto* req_ids = Require(req_ctx, kRawRequestIds, -8501);
    const auto* policies = Require(req_ctx, kMatchedPolicyClauses, -8501);
    const auto* verdicts = Require(req_ctx, kGeneratedVerdicts, -8501);
    const auto* rerank_scores = req_ctx.Get(kRerankScores);

    if (!req_ids || !policies || !verdicts) return -8501;

    size_t batch_size = req_ids->size();
    std::vector<DialogueAuditResult> outputs(batch_size);

    for (size_t i = 0; i < batch_size; ++i) {
      outputs[i].request_id = (*req_ids)[i];
      outputs[i].status_code = 0;
      outputs[i].risk_score = (rerank_scores && i < rerank_scores->size())
                                  ? (*rerank_scores)[i]
                                  : 0.0f;
      outputs[i].risk_level = "SAFE";
      outputs[i].matched_policy_clause = (*policies)[i];
    }

    // 从 LLM 输出解析结构化字段
    for (const auto& item : *verdicts) {
      uint32_t r_id = item.req_id;
      if (r_id < batch_size) {
        outputs[r_id].audit_verdict_json = item.data;

        try {
          nlohmann::json j = nlohmann::json::parse(item.data);
          std::string level = j.value("risk_level", "SAFE");
          float score = j.value("risk_score", outputs[r_id].risk_score);
          outputs[r_id].risk_score = score;
          outputs[r_id].risk_level = level;
        } catch (...) {
        }
      }
    }

    std::cout << "[AuditPostNode] Finalized " << batch_size
              << " compliance audit results." << std::endl;
    Publish(req_ctx, kComplianceAuditOutputs, std::move(outputs));
    return 0;
  }
};

NodeDefinition MakeAuditPostNodeDefinition() {
  NodeDefinition def;
  def.node_type = AuditPostNode::kNodeType;
  def.category = "biz";
  def.description = "Dialogue audit result packaging and post-processing node";
  def.inputs = {
      RequiredInput(kRawRequestIds), RequiredInput(kMatchedPolicyClauses),
      OptionalInput(kRerankScores), RequiredInput(kGeneratedVerdicts)};
  def.outputs = {Output(kComplianceAuditOutputs)};
  def.biz_names = {kDialogueAuditBizName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(AuditPostNode, MakeAuditPostNodeDefinition());

}  // namespace alg_framework

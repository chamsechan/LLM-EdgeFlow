#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "biz/dialogue_audit/dialogue_audit_contract.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"
#include "nodes/model_bound_node.h"

namespace alg_framework {

/**
 * @brief Cross-Encoder 深度语义精排算子 (Node 3: 使用 Model 2 - Rerank 引擎)
 */
class CrossRerankNode final : public ModelBoundNode<IRerankEngine> {
 public:
  inline static constexpr char kNodeType[] = "CrossRerankNode";

  CrossRerankNode()
      : ModelBoundNode<IRerankEngine>(kNodeType, "rerank_model_v1") {}

 protected:
  int ProcessNode(AlgContext& req_ctx) override {
    const auto* user_texts = Require(req_ctx, kUserTexts, -8201);
    const auto* candidate_policies =
        Require(req_ctx, kCandidatePolicies, -8201);

    if (!user_texts || !candidate_policies) return -8201;

    // 构造带溯源标签的 (Query, Policy) 样本对
    std::vector<TraceableItem<IRerankEngine::PairInput>> pair_items;
    for (const auto& item : *candidate_policies) {
      uint32_t r_id = item.req_id;
      const std::string& query = (*user_texts)[r_id];
      for (uint32_t s_id = 0; s_id < item.data.size(); ++s_id) {
        pair_items.emplace_back(
            r_id, s_id, IRerankEngine::PairInput{query, item.data[s_id]});
      }
    }

    std::vector<TraceableItem<float>> pair_scores;
    std::cout << "[CrossRerankNode] Invoking Cross-Encoder scoring on "
              << pair_items.size() << " candidate pairs..." << std::endl;
    int ret = engine()->ScoreTraceableBatch(pair_items, &pair_scores);
    if (ret != 0) {
      return Fail(req_ctx, ret,
                  "CrossRerankNode: Cross-Encoder scoring failed");
    }

    // 为每个 req_id 选出精排得分最高的最匹配政策条款
    std::vector<std::string> best_policy_clauses(user_texts->size(), "");
    std::vector<float> best_policy_scores(user_texts->size(), 0.0f);

    for (const auto& score_item : pair_scores) {
      uint32_t r_id = score_item.req_id;
      uint32_t s_id = score_item.sub_id;
      if (r_id < user_texts->size() &&
          score_item.data > best_policy_scores[r_id]) {
        best_policy_scores[r_id] = score_item.data;
        best_policy_clauses[r_id] = (*candidate_policies)[r_id].data[s_id];
      }
    }

    Publish(req_ctx, kMatchedPolicyClauses, std::move(best_policy_clauses));
    Publish(req_ctx, kRerankScores, std::move(best_policy_scores));
    return 0;
  }
};

NodeDefinition MakeCrossRerankNodeDefinition() {
  NodeDefinition def;
  def.node_type = CrossRerankNode::kNodeType;
  def.category = "biz";
  def.description = "Cross-encoder policy reranking node";
  def.inputs = {RequiredInput(kUserTexts), RequiredInput(kCandidatePolicies)};
  def.outputs = {Output(kMatchedPolicyClauses), Output(kRerankScores)};
  def.config_fields = {ConfigFieldDefinition{
      "bind_model", ConfigValueKind::kString, false, "rerank_model_v1"}};
  def.model_capability = "rerank";
  def.model_config_field = "bind_model";
  def.biz_names = {kDialogueAuditBizName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(CrossRerankNode, MakeCrossRerankNodeDefinition());

}  // namespace alg_framework

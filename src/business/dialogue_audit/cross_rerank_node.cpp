#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "business/dialogue_audit/dialogue_audit_contract.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"

namespace alg_framework {

/**
 * @brief Cross-Encoder 深度语义精排算子 (Node 3: 使用 Model 2 - Rerank 引擎)
 */
class CrossRerankNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "CrossRerankNode";

  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    std::string bind_model = config.value("bind_model", "rerank_model_v1");
    rerank_engine_ =
        session_ctx->GetModelManager().GetModel<IRerankEngine>(bind_model);
    if (!rerank_engine_) {
      std::cerr << "[CrossRerankNode] Failed to bind model: " << bind_model
                << std::endl;
      return false;
    }
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* user_texts = req_ctx->Get(kUserTexts);
    auto* candidate_policies = req_ctx->Get(kCandidatePolicies);

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
    int ret = rerank_engine_->ScoreTraceableBatch(pair_items, &pair_scores);
    if (ret != 0) return ret;

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

    req_ctx->Set(kMatchedPolicyClauses, std::move(best_policy_clauses));
    req_ctx->Set(kRerankScores, std::move(best_policy_scores));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = kNodeType;
    return name;
  }

 private:
  std::shared_ptr<IRerankEngine> rerank_engine_;
};

NodeDefinition MakeCrossRerankNodeDefinition() {
  NodeDefinition def;
  def.node_type = CrossRerankNode::kNodeType;
  def.category = "business";
  def.description = "Cross-encoder policy reranking node";
  def.inputs = {RequiredInput(kUserTexts), RequiredInput(kCandidatePolicies)};
  def.outputs = {Output(kMatchedPolicyClauses), Output(kRerankScores)};
  def.config_fields = {
      ConfigFieldDefinition{"bind_model", ConfigValueKind::kString, true}};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(CrossRerankNode, MakeCrossRerankNodeDefinition());

}  // namespace alg_framework

#include <algorithm>
#include <cmath>
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
 * @brief 合规政策库向量初筛检索算子 (Node 2: 使用 Model 1 - Embedding 引擎)
 */
class DenseRetrievalNode final : public ModelBoundNode<IEmbeddingEngine> {
 public:
  inline static constexpr char kNodeType[] = "DenseRetrievalNode";

  DenseRetrievalNode()
      : ModelBoundNode<IEmbeddingEngine>(kNodeType, "embed_model_v2") {}

 protected:
  bool InitModelNode(const nlohmann::json& config,
                     SessionContext& /*session_ctx*/) override {
    top_k_ = config.value("top_k", 4);

    // 初始化合规知识库条款候选
    policy_database_ = {
        "【合规条款 "
        "101】严禁诱导用户脱离平台进行私下转账与虚假交易，违者封禁账号并追究法"
        "律责任。",
        "【合规条款 "
        "102】严禁索取用户银行卡密码、CVV码与短信验证码等高风险隐私信息。",
        "【合规条款 "
        "103】客服人员应保持专业礼貌，严禁使用侮辱、谩骂或威胁性语言对待客户。",
        "【合规条款 "
        "104】支持7天无理由退货与正常售后申诉，禁止以任何理由恶意推诿延误退款"
        "。"};

    std::vector<TraceableItem<std::string>> policy_items;
    for (uint32_t i = 0; i < policy_database_.size(); ++i) {
      policy_items.emplace_back(0, i, policy_database_[i]);
    }
    int ret = engine()->InferTraceableBatch(policy_items, &policy_embeddings_);
    return ret == 0;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* user_texts = Require(req_ctx, kUserTexts, -8101);
    if (!user_texts) return -8101;

    std::vector<TraceableItem<std::string>> query_items;
    for (uint32_t req_id = 0; req_id < user_texts->size(); ++req_id) {
      query_items.emplace_back(req_id, 0, (*user_texts)[req_id]);
    }

    std::vector<TraceableItem<std::vector<float>>> query_embeddings;
    int ret = engine()->InferTraceableBatch(query_items, &query_embeddings);
    if (ret != 0) {
      return Fail(req_ctx, ret,
                  "DenseRetrievalNode: Query embedding inference failed");
    }

    // 基于 Query Embedding 与政策库 Embedding 的余弦相似度计算与 Top-K 初筛
    std::vector<TraceableItem<std::vector<std::string>>> candidate_policies(
        user_texts->size());
    for (uint32_t req_id = 0; req_id < user_texts->size(); ++req_id) {
      const auto& q_emb = query_embeddings[req_id].data;
      std::vector<std::pair<float, size_t>> scored_policies;
      scored_policies.reserve(policy_database_.size());

      for (size_t p_idx = 0; p_idx < policy_database_.size(); ++p_idx) {
        float sim =
            (p_idx < policy_embeddings_.size())
                ? CosineSimilarity(q_emb, policy_embeddings_[p_idx].data)
                : 0.0f;
        scored_policies.emplace_back(sim, p_idx);
      }

      std::sort(scored_policies.begin(), scored_policies.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });

      size_t count =
          std::min(static_cast<size_t>(top_k_), scored_policies.size());
      std::vector<std::string> retrieved;
      retrieved.reserve(count);
      for (size_t i = 0; i < count; ++i) {
        retrieved.push_back(policy_database_[scored_policies[i].second]);
      }

      candidate_policies[req_id] = TraceableItem<std::vector<std::string>>(
          req_id, 0, std::move(retrieved));
    }

    Publish(req_ctx, kCandidatePolicies, std::move(candidate_policies));
    return 0;
  }

 private:
  static float CosineSimilarity(const std::vector<float>& a,
                                const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) return 0.0f;
    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
      dot += a[i] * b[i];
      norm_a += a[i] * a[i];
      norm_b += b[i] * b[i];
    }
    if (norm_a <= 0.0f || norm_b <= 0.0f) return 0.0f;
    return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
  }

  int top_k_ = 4;
  std::vector<std::string> policy_database_;
  std::vector<TraceableItem<std::vector<float>>> policy_embeddings_;
};

NodeDefinition MakeDenseRetrievalNodeDefinition() {
  NodeDefinition def;
  def.node_type = DenseRetrievalNode::kNodeType;
  def.category = "biz";
  def.description = "Dense policy retrieval node";
  def.inputs = {RequiredInput(kUserTexts)};
  def.outputs = {Output(kCandidatePolicies)};
  def.config_fields = {
      ConfigFieldDefinition{"bind_model", ConfigValueKind::kString, false,
                            "embed_model_v2"},
      ConfigFieldDefinition{"top_k", ConfigValueKind::kInteger, false, 4, 1.0,
                            100.0}};
  def.model_capability = "embedding";
  def.model_config_field = "bind_model";
  def.biz_names = {kDialogueAuditBizName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(DenseRetrievalNode,
                              MakeDenseRetrievalNodeDefinition());

}  // namespace alg_framework

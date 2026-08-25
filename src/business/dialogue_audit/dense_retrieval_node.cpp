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
 * @brief 合规政策库向量初筛检索算子 (Node 2: 使用 Model 1 - Embedding 引擎)
 */
class DenseRetrievalNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "DenseRetrievalNode";

  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    std::string bind_model = config.value("bind_model", "embed_model_v2");
    embed_engine_ =
        session_ctx->GetModelManager().GetModel<IEmbeddingEngine>(bind_model);
    if (!embed_engine_) {
      std::cerr << "[DenseRetrievalNode] Failed to bind model: " << bind_model
                << std::endl;
      return false;
    }

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

    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* user_texts = req_ctx->Get(kUserTexts);
    if (!user_texts) return -8101;

    std::vector<TraceableItem<std::string>> query_items;
    for (uint32_t req_id = 0; req_id < user_texts->size(); ++req_id) {
      query_items.emplace_back(req_id, 0, (*user_texts)[req_id]);
    }

    std::vector<TraceableItem<std::vector<float>>> query_embeddings;
    int ret =
        embed_engine_->InferTraceableBatch(query_items, &query_embeddings);
    if (ret != 0) return ret;

    // 为每个请求召回全部政策条款作为精排候选对
    std::vector<TraceableItem<std::vector<std::string>>> candidate_policies(
        user_texts->size());
    for (uint32_t req_id = 0; req_id < user_texts->size(); ++req_id) {
      candidate_policies[req_id] =
          TraceableItem<std::vector<std::string>>(req_id, 0, policy_database_);
    }

    req_ctx->Set(kCandidatePolicies, std::move(candidate_policies));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = kNodeType;
    return name;
  }

 private:
  std::shared_ptr<IEmbeddingEngine> embed_engine_;
  std::vector<std::string> policy_database_;
};

NodeDefinition MakeDenseRetrievalNodeDefinition() {
  NodeDefinition def;
  def.node_type = DenseRetrievalNode::kNodeType;
  def.category = "business";
  def.description = "Dense policy retrieval node";
  def.inputs = {RequiredInput(kUserTexts)};
  def.outputs = {Output(kCandidatePolicies)};
  def.config_fields = {
      ConfigFieldDefinition{"bind_model", ConfigValueKind::kString, true}};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(DenseRetrievalNode,
                              MakeDenseRetrievalNodeDefinition());

}  // namespace alg_framework

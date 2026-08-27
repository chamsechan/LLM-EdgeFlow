#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "biz/dialogue_audit/dialogue_audit_contract.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "nodes/node_support.h"

namespace alg_framework {

/**
 * @brief 风控前置硬规则快筛算子 (Node 1: 内存私有敏感词库)
 */
class SafetyRulePreNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "SafetyRulePreNode";

  SafetyRulePreNode() : NodeBase(kNodeType) {}

 protected:
  bool InitNode(const nlohmann::json& config,
                SessionContext& /*session_ctx*/) override {
    if (config.contains("blacklist") && config["blacklist"].is_array()) {
      for (const auto& item : config["blacklist"]) {
        blacklist_.insert(item.get<std::string>());
      }
    }
    std::cout << "[SafetyRulePreNode] Loaded " << blacklist_.size()
              << " hard-risk keywords into node private memory." << std::endl;
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* user_texts = Require(req_ctx, kUserTexts, -8001);
    if (!user_texts) {
      return -8001;
    }

    std::vector<bool> hard_risk_flags(user_texts->size(), false);
    std::vector<std::string> hit_keywords(user_texts->size(), "");

    for (size_t i = 0; i < user_texts->size(); ++i) {
      const std::string& text = (*user_texts)[i];
      for (const auto& kw : blacklist_) {
        if (text.find(kw) != std::string::npos) {
          hard_risk_flags[i] = true;
          hit_keywords[i] = kw;
          break;
        }
      }
    }

    Publish(req_ctx, kHardRiskFlags, std::move(hard_risk_flags));
    Publish(req_ctx, kHitKeywords, std::move(hit_keywords));
    return 0;
  }

 private:
  std::unordered_set<std::string> blacklist_;
};

NodeDefinition MakeSafetyRulePreNodeDefinition() {
  NodeDefinition def;
  def.node_type = SafetyRulePreNode::kNodeType;
  def.category = "biz";
  def.description = "Dialogue safety hard rule pre-processing node";
  def.inputs = {RequiredInput(kUserTexts)};
  def.outputs = {Output(kHardRiskFlags), Output(kHitKeywords)};
  def.config_fields = {
      ConfigFieldDefinition{"blacklist", ConfigValueKind::kArray, false}};
  def.biz_names = {kDialogueAuditBizName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(SafetyRulePreNode,
                              MakeSafetyRulePreNodeDefinition());

}  // namespace alg_framework

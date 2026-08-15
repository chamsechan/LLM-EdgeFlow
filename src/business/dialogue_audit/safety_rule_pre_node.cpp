#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"

namespace alg_framework {

/**
 * @brief 风控前置硬规则快筛算子 (Node 1: 内存私有敏感词库)
 */
class SafetyRulePreNode : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    (void)session_ctx;
    if (config.contains("blacklist") && config["blacklist"].is_array()) {
      for (const auto& item : config["blacklist"]) {
        blacklist_.insert(item.get<std::string>());
      }
    }
    std::cout << "[SafetyRulePreNode] Loaded " << blacklist_.size()
              << " hard-risk keywords into node private memory." << std::endl;
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* user_texts = req_ctx->Get<std::vector<std::string>>("user_texts");
    if (!user_texts) {
      req_ctx->SetError(-8001, "SafetyRulePreNode: Missing user_texts");
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

    req_ctx->Set("hard_risk_flags", std::move(hard_risk_flags));
    req_ctx->Set("hit_keywords", std::move(hit_keywords));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "SafetyRulePreNode";
    return name;
  }

 private:
  std::unordered_set<std::string> blacklist_;
};

REGISTER_NODE(SafetyRulePreNode);

}  // namespace alg_framework

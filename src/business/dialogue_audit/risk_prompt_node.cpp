#include <iostream>
#include <string>
#include <vector>

#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"

namespace alg_framework {

/**
 * @brief 风控质检 Prompt 组装算子 (Node 4: 汇聚规则与精排条款)
 */
class RiskPromptNode : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    (void)session_ctx;
    prompt_tpl_ = config.value(
        "template",
        "你是一个专业的智能客服合规风控质检大模型。请根据相关制度条款对客服与用"
        "户的对话进行合规判定并给出建议：\n"
        "【对话内容】: {user_text}\n"
        "【相关制度】: {policy}\n"
        "【规则初筛】: {rule_flag}\n"
        "请严格按JSON格式输出判定：{\"verdict\": \"违规/合规\", "
        "\"risk_level\": \"HIGH_RISK/SAFE\", \"suggestion\": \"...\"}\n"
        "【审核结论】:");
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* user_texts = req_ctx->Get<std::vector<std::string>>("user_texts");
    auto* policies =
        req_ctx->Get<std::vector<std::string>>("matched_policy_clauses");
    auto* hard_flags = req_ctx->Get<std::vector<bool>>("hard_risk_flags");

    if (!user_texts || !policies) return -8301;

    std::vector<TraceableItem<std::string>> llm_prompts;
    for (uint32_t req_id = 0; req_id < user_texts->size(); ++req_id) {
      std::string prompt = prompt_tpl_;

      size_t pos = prompt.find("{user_text}");
      if (pos != std::string::npos)
        prompt.replace(pos, 11, (*user_texts)[req_id]);

      pos = prompt.find("{policy}");
      if (pos != std::string::npos) prompt.replace(pos, 8, (*policies)[req_id]);

      bool is_flagged = (hard_flags && req_id < hard_flags->size())
                            ? (*hard_flags)[req_id]
                            : false;
      pos = prompt.find("{rule_flag}");
      if (pos != std::string::npos)
        prompt.replace(pos, 11,
                       is_flagged ? "已命中敏感词硬规则" : "未命中硬规则");

      llm_prompts.emplace_back(req_id, 0, std::move(prompt));
    }

    req_ctx->Set("llm_audit_prompts", std::move(llm_prompts));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "RiskPromptNode";
    return name;
  }

 private:
  std::string prompt_tpl_;
};

REGISTER_NODE(RiskPromptNode);

}  // namespace alg_framework

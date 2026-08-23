#include <iostream>

#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"

namespace alg_framework {

/**
 * @brief 通用 Prompt 模板组装算子
 */
class PromptBuilderNode : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    (void)session_ctx;
    template_str_ =
        config.value("template", "Context: {context}\nQuery: {query}\nAnswer:");
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* matched_contexts =
        req_ctx->Get<std::vector<TraceableItem<std::string>>>(
            "matched_top_chunks");
    auto* raw_queries = req_ctx->Get<std::vector<std::string>>("raw_queries");

    if (!matched_contexts || !raw_queries) {
      req_ctx->SetError(-3001, "PromptBuilderNode: Missing inputs");
      return -3001;
    }

    std::vector<TraceableItem<std::string>> final_prompts;
    final_prompts.reserve(matched_contexts->size());

    for (const auto& item : *matched_contexts) {
      std::string query = (item.req_id < raw_queries->size())
                              ? (*raw_queries)[item.req_id]
                              : "";
      std::string prompt = template_str_;

      // 简单模板字符串替换
      size_t pos = prompt.find("{context}");
      if (pos != std::string::npos) prompt.replace(pos, 9, item.data);
      pos = prompt.find("{query}");
      if (pos != std::string::npos) prompt.replace(pos, 7, query);

      final_prompts.emplace_back(item.req_id, item.sub_id, std::move(prompt));
    }

    req_ctx->Set("llm_input_prompts", std::move(final_prompts));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "PromptBuilderNode";
    return name;
  }

 private:
  std::string template_str_;
};

REGISTER_NODE(PromptBuilderNode);

}  // namespace alg_framework

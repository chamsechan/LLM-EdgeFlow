#include <iostream>

#include "biz/doc_qa/doc_qa_contract.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "nodes/node_support.h"

namespace alg_framework {

/**
 * @brief Prompt 模板组装算子
 */
class PromptBuilderNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "PromptBuilderNode";

  PromptBuilderNode() : NodeBase(kNodeType) {}

 protected:
  bool InitNode(const nlohmann::json& config,
                SessionContext& /*session_ctx*/) override {
    template_str_ =
        config.value("template", "Context: {context}\nQuery: {query}\nAnswer:");
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* matched_contexts = Require(req_ctx, kMatchedTopChunks, -3001);
    const auto* raw_queries = Require(req_ctx, kRawQueries, -3001);

    if (!matched_contexts || !raw_queries) {
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

    Publish(req_ctx, kLlmInputPrompts, std::move(final_prompts));
    return 0;
  }

 private:
  std::string template_str_;
};

NodeDefinition MakePromptBuilderNodeDefinition() {
  NodeDefinition def;
  def.node_type = PromptBuilderNode::kNodeType;
  def.category = "biz";
  def.description = "Prompt builder node";
  def.inputs = {RequiredInput(kMatchedTopChunks), RequiredInput(kRawQueries)};
  def.outputs = {Output(kLlmInputPrompts)};
  def.config_fields = {
      ConfigFieldDefinition{"template", ConfigValueKind::kString, false,
                            "Context: {context}\nQuery: {query}\nAnswer:"}};
  def.biz_names = {kDocQaBizName, kDocQaOnnxBizName, kDocQaRerankBizName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(PromptBuilderNode,
                              MakePromptBuilderNodeDefinition());

}  // namespace alg_framework

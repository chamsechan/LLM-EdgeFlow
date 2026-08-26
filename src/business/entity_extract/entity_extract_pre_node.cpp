#include <iostream>
#include <string>
#include <vector>

#include "business/entity_extract/entity_extract_contract.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "nodes/node_support.h"

namespace alg_framework {

/**
 * @brief 实体提取前处理算子 (构造 0.6B 实体/名词提取 Prompt)
 */
class EntityExtractPreNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "EntityExtractPreNode";

  EntityExtractPreNode() : NodeBase(kNodeType) {}

 protected:
  bool InitNode(const nlohmann::json& config,
                SessionContext& /*session_ctx*/) override {
    prompt_template_ = config.value(
        "prompt_template",
        "你是一个中文实体与名词抽取助手。请从以下句子中提取出所有名词与实体，"
        "并仅以JSON列表形式返回：\n输入文本：{text}\n提取结果：");
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* sentences = Require(req_ctx, kInputSentences, -6001);
    if (!sentences) {
      return -6001;
    }

    std::vector<TraceableItem<std::string>> prompt_items;
    prompt_items.reserve(sentences->size());

    for (uint32_t req_id = 0; req_id < sentences->size(); ++req_id) {
      std::string prompt = prompt_template_;
      size_t pos = prompt.find("{text}");
      if (pos != std::string::npos) {
        prompt.replace(pos, 6, (*sentences)[req_id]);
      }
      prompt_items.emplace_back(req_id, 0, std::move(prompt));
    }

    std::cout << "[EntityExtractPreNode] Formatted " << prompt_items.size()
              << " prompts for 0.6B model." << std::endl;

    Publish(req_ctx, kLlmInputPrompts, std::move(prompt_items));
    return 0;
  }

 private:
  std::string prompt_template_;
};

NodeDefinition MakeEntityExtractPreNodeDefinition() {
  NodeDefinition def;
  def.node_type = EntityExtractPreNode::kNodeType;
  def.category = "business";
  def.description = "Entity extract prompt builder pre-processing node";
  def.inputs = {RequiredInput(kInputSentences)};
  def.outputs = {Output(kLlmInputPrompts)};
  def.config_fields = {ConfigFieldDefinition{
      "prompt_template", ConfigValueKind::kString, false,
      "你是一个中文实体与名词抽取助手。请从以下句子中提取出所有名词与实体，"
      "并仅以JSON列表形式返回：\n输入文本：{text}\n提取结果："}};
  def.business_names = {kEntityExtractBusinessName,
                        kEntityExtractLlamaCppBusinessName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(EntityExtractPreNode,
                              MakeEntityExtractPreNodeDefinition());

}  // namespace alg_framework

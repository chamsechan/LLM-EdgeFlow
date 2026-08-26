#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "business/doc_qa/doc_qa_contract.h"
#include "core/node_registry.h"
#include "nodes/node_support.h"

namespace alg_framework {

/**
 * @brief 意图识别规则算子 (展示开发者有状态类的私有数据存储)
 *
 * 开发者可在此类中自由定义成员变量：
 * - 意图词表映射 (intent_keywords_map_)
 * - 置信度阈值 (threshold_)
 * - 私有计数器或缓存结构
 */
class IntentRuleNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "IntentRuleNode";

  IntentRuleNode() : NodeBase(kNodeType) {}

 protected:
  bool InitNode(const nlohmann::json& config,
                SessionContext& /*session_ctx*/) override {
    threshold_ = config.value("threshold", 0.75f);
    default_intent_ = config.value("default_intent", "GENERAL_CONSULT");

    // 从 JSON 配置加载私有业务规则字典存放到本实例成员变量中
    if (config.contains("rules") && config["rules"].is_object()) {
      for (auto& [intent, keywords] : config["rules"].items()) {
        std::vector<std::string> kw_list;
        for (const auto& kw : keywords) {
          kw_list.push_back(kw.get<std::string>());
        }
        intent_keywords_map_[intent] = kw_list;
      }
    }

    std::cout << "[IntentRuleNode] Loaded " << intent_keywords_map_.size()
              << " custom intent rules into node private state." << std::endl;
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* raw_queries = Require(req_ctx, kRawQueries, -4201);
    if (!raw_queries) {
      return -4201;
    }

    std::vector<std::string> recognized_intents(raw_queries->size(),
                                                default_intent_);
    std::vector<float> confidences(raw_queries->size(), 0.5f);

    // 针对每个请求进行业务规则匹配
    for (size_t req_id = 0; req_id < raw_queries->size(); ++req_id) {
      const std::string& q = (*raw_queries)[req_id];
      for (const auto& [intent, kw_list] : intent_keywords_map_) {
        for (const auto& kw : kw_list) {
          if (q.find(kw) != std::string::npos) {
            recognized_intents[req_id] = intent;
            confidences[req_id] = 0.95f;
            break;
          }
        }
      }
    }

    Publish(req_ctx, kRecognizedIntents, std::move(recognized_intents));
    Publish(req_ctx, kIntentConfidences, std::move(confidences));
    return 0;
  }

 private:
  std::unordered_map<std::string, std::vector<std::string>>
      intent_keywords_map_;
  float threshold_ = 0.75f;
  std::string default_intent_ = "GENERAL_CONSULT";
};

NodeDefinition MakeIntentRuleNodeDefinition() {
  NodeDefinition def;
  def.node_type = IntentRuleNode::kNodeType;
  def.category = "business";
  def.description = "Document QA intent recognition rule node";
  def.inputs = {RequiredInput(kRawQueries)};
  def.outputs = {Output(kRecognizedIntents), Output(kIntentConfidences)};
  def.config_fields = {
      ConfigFieldDefinition{"threshold", ConfigValueKind::kNumber, false, 0.75,
                            0.0, 1.0},
      ConfigFieldDefinition{"default_intent", ConfigValueKind::kString, false,
                            "GENERAL_CONSULT"},
      ConfigFieldDefinition{"rules", ConfigValueKind::kObject, false,
                            nlohmann::json::object()}};
  def.business_names = {kDocQaBusinessName, kDocQaOnnxBusinessName,
                        kDocQaRerankBusinessName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(IntentRuleNode, MakeIntentRuleNodeDefinition());

}  // namespace alg_framework

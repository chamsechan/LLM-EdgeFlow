#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/node_base.h"
#include "core/node_registry.h"

namespace alg_framework {

/**
 * @brief 意图识别规则算子 (展示开发者有状态类的私有数据存储)
 *
 * 开发者可在此类中自由定义成员变量：
 * - 意图词表映射 (intent_keywords_map_)
 * - 置信度阈值 (threshold_)
 * - 私有计数器或缓存结构
 */
class IntentRuleNode : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
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

  int Process(AlgContext* req_ctx) override {
    auto* raw_queries = req_ctx->Get<std::vector<std::string>>("raw_queries");
    if (!raw_queries) {
      req_ctx->SetError(-4201, "IntentRuleNode: Missing raw_queries");
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

    req_ctx->Set("recognized_intents", std::move(recognized_intents));
    req_ctx->Set("intent_confidences", std::move(confidences));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "IntentRuleNode";
    return name;
  }

 private:
  // ==========================================
  // 开发者私有数据成员 (随节点实例常驻内存)
  // ==========================================
  std::unordered_map<std::string, std::vector<std::string>>
      intent_keywords_map_;
  float threshold_ = 0.75f;
  std::string default_intent_ = "GENERAL_CONSULT";
};

REGISTER_NODE(IntentRuleNode);

}  // namespace alg_framework

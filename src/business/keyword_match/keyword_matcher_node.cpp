#include <cstring>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "business/keyword_match/keyword_match_contract.h"
#include "business/keyword_match/keyword_match_dto.h"
#include "core/node_registry.h"
#include "nodes/node_support.h"

namespace alg_framework {

/**
 * @brief 关注词匹配算子 (无需加载模型，纯规则与内存字典)
 */
class KeywordMatcherNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "KeywordMatcherNode";

  KeywordMatcherNode() : NodeBase(kNodeType) {}

  int Control(int cmd, const std::string& json_param) override {
    if (cmd == 1) {  // 1: 更新关注词表
      try {
        nlohmann::json root = nlohmann::json::parse(json_param);
        if (root.contains("categories") && root["categories"].is_object()) {
          UpdateCategoryKeywords(root["categories"]);
          std::cout << "[KeywordMatcherNode] Dynamically updated categories "
                       "via Control! Total categories: "
                    << category_keywords_map_.size() << std::endl;
          return 0;
        }
      } catch (const std::exception& e) {
        std::cerr << "[KeywordMatcherNode] Control JSON parse error: "
                  << e.what() << std::endl;
        return -1;
      }
    }
    return 0;
  }

 protected:
  bool InitNode(const nlohmann::json& config,
                SessionContext& /*session_ctx*/) override {
    // 从初始化配置加载默认词表 (可选)
    if (config.contains("default_categories") &&
        config["default_categories"].is_object()) {
      UpdateCategoryKeywords(config["default_categories"]);
    }
    std::cout << "[KeywordMatcherNode] Initialized with "
              << category_keywords_map_.size() << " initial categories."
              << std::endl;
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* sentences = Require(req_ctx, kInputSentences, -5001);
    const auto* req_ids = Require(req_ctx, kRawRequestIds, -5001);

    if (!sentences || !req_ids) {
      return -5001;
    }

    size_t batch_size = sentences->size();
    std::vector<KeywordMatchResult> outputs(batch_size);

    std::shared_lock<std::shared_mutex> lock(rw_mutex_);

    for (size_t i = 0; i < batch_size; ++i) {
      outputs[i].request_id = (*req_ids)[i];
      outputs[i].status_code = 0;

      const std::string& sentence = (*sentences)[i];
      nlohmann::json matches_array = nlohmann::json::array();

      // 在当前词表中遍历匹配
      for (const auto& [category, words] : category_keywords_map_) {
        for (const auto& w : words) {
          if (sentence.find(w) != std::string::npos) {
            nlohmann::json match_item;
            match_item["category"] = category;
            match_item["matched_word"] = w;
            matches_array.push_back(match_item);
          }
        }
      }

      nlohmann::json result_json;
      if (matches_array.empty()) {
        // 未命中：输出空列表 JSON
        outputs[i].is_hit = 0;
        result_json["matches"] = nlohmann::json::array();
      } else {
        // 命中：输出命中详情
        outputs[i].is_hit = 1;
        result_json["matches"] = matches_array;
      }

      outputs[i].match_result_json = result_json.dump();
    }

    std::cout << "[KeywordMatcherNode] Processed batch of " << batch_size
              << " sentences." << std::endl;
    Publish(req_ctx, kKeywordMatchOutputs, std::move(outputs));
    return 0;
  }

 private:
  void UpdateCategoryKeywords(const nlohmann::json& categories_json) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    category_keywords_map_.clear();
    for (auto it = categories_json.begin(); it != categories_json.end(); ++it) {
      if (it.value().is_array()) {
        std::vector<std::string> words;
        for (const auto& w : it.value()) {
          if (w.is_string()) {
            words.push_back(w.get<std::string>());
          }
        }
        category_keywords_map_[it.key()] = std::move(words);
      }
    }
  }

  mutable std::shared_mutex rw_mutex_;
  // 节点私有词表映射：类别 -> 词列表
  std::unordered_map<std::string, std::vector<std::string>>
      category_keywords_map_;
};

NodeDefinition MakeKeywordMatcherNodeDefinition() {
  NodeDefinition def;
  def.node_type = KeywordMatcherNode::kNodeType;
  def.category = "business";
  def.description = "Keyword matcher node";
  def.inputs = {RequiredInput(kInputSentences), RequiredInput(kRawRequestIds)};
  def.outputs = {Output(kKeywordMatchOutputs)};
  def.config_fields = {ConfigFieldDefinition{"default_categories",
                                             ConfigValueKind::kObject, false}};
  def.business_names = {kKeywordMatchBusinessName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(KeywordMatcherNode,
                              MakeKeywordMatcherNodeDefinition());

}  // namespace alg_framework

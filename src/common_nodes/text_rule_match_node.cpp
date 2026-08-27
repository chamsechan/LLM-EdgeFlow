#include <algorithm>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "nodes/node_support.h"

namespace alg_framework {

/**
 * @brief 规则与关键词匹配通用算子 (TextRuleMatchNode,
 * 支持动态热更新词表与线程安全快照)
 */
class TextRuleMatchNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "TextRuleMatchNode";

  TextRuleMatchNode()
      : NodeBase(kNodeType),
        in_text_("text", "text", "TextBatch"),
        out_matches_("matches", "matches", "RuleMatchBatch") {}

  NodeControlResult ControlNode(int cmd,
                                const std::string& json_param) override {
    if (cmd == 1) {  // 1: 动态更新关注词/规则表
      try {
        nlohmann::json root = nlohmann::json::parse(json_param);
        if (root.contains("categories") && root["categories"].is_object()) {
          UpdateCategories(root["categories"]);
          return NodeControlResult::Handled(
              0, "TextRuleMatchNode categories updated");
        }
      } catch (const std::exception& e) {
        return NodeControlResult::Failed(
            -1, std::string("JSON parse error in Control: ") + e.what());
      }
    }
    return NodeControlResult::Unsupported();
  }

 protected:
  bool InitNode(const nlohmann::json& config,
                SessionContext& /*session_ctx*/) override {
    BindPort(in_text_);
    BindPort(out_matches_);

    if (config.contains("default_categories") &&
        config["default_categories"].is_object()) {
      UpdateCategories(config["default_categories"]);
    } else if (config.contains("categories") &&
               config["categories"].is_object()) {
      UpdateCategories(config["categories"]);
    }
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* text_items =
        in_text_.Require(req_ctx, -5001, "TextRuleMatchNode input");
    if (!text_items) {
      return -5001;
    }

    std::shared_lock<std::shared_mutex> lock(rw_mutex_);

    RuleMatchBatch output_matches;
    output_matches.reserve(text_items->size());

    for (const auto& item : *text_items) {
      const std::string& sentence = item.data;
      uint32_t req_id = item.req_id;
      uint32_t sub_id = item.sub_id;

      nlohmann::json matches_array = nlohmann::json::array();
      std::string first_hit_category;
      std::string first_hit_word;
      int is_hit = 0;

      for (const auto& [category, words] : category_keywords_list_) {
        for (const auto& w : words) {
          if (w.empty()) continue;
          if (sentence.find(w) != std::string::npos) {
            is_hit = 1;
            if (first_hit_category.empty()) {
              first_hit_category = category;
              first_hit_word = w;
            }
            nlohmann::json match_elem;
            match_elem["category"] = category;
            match_elem["matched_word"] = w;
            matches_array.push_back(std::move(match_elem));
          }
        }
      }

      nlohmann::json result_json;
      result_json["matches"] = matches_array;
      if (is_hit && !first_hit_category.empty()) {
        result_json["intent"] = first_hit_category;
        result_json["matched_word"] = first_hit_word;
      }

      RuleMatchItem match_item(is_hit, first_hit_category, first_hit_word,
                               result_json.dump(), is_hit ? 1.0f : 0.0f);
      match_item.details = result_json;

      output_matches.emplace_back(req_id, sub_id, std::move(match_item));
    }

    out_matches_.Set(req_ctx, std::move(output_matches));
    return 0;
  }

 private:
  void UpdateCategories(const nlohmann::json& categories_json) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    category_keywords_list_.clear();
    for (auto it = categories_json.begin(); it != categories_json.end(); ++it) {
      if (it.value().is_array()) {
        std::vector<std::string> words;
        for (const auto& w : it.value()) {
          if (w.is_string()) {
            words.push_back(w.get<std::string>());
          }
        }
        category_keywords_list_.push_back({it.key(), std::move(words)});
      }
    }
  }

  mutable std::shared_mutex rw_mutex_;
  std::vector<std::pair<std::string, std::vector<std::string>>>
      category_keywords_list_;

  BoundInput<TextBatch> in_text_;
  BoundOutput<RuleMatchBatch> out_matches_;
};

NodeDefinition MakeTextRuleMatchNodeDefinition() {
  NodeDefinition def;
  def.node_type = TextRuleMatchNode::kNodeType;
  def.category = "common";
  def.description = "Keyword and rule matching engine node";
  def.inputs = {
      RequiredInputPort("text", BlackboardKey<TextBatch>{"", "TextBatch"})};
  def.outputs = {OutputPort(
      "matches", BlackboardKey<RuleMatchBatch>{"", "RuleMatchBatch"})};
  def.config_fields = {
      ConfigFieldDefinition{"default_categories", ConfigValueKind::kObject,
                            false},
      ConfigFieldDefinition{"categories", ConfigValueKind::kObject, false}};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(TextRuleMatchNode,
                              MakeTextRuleMatchNodeDefinition());

}  // namespace alg_framework

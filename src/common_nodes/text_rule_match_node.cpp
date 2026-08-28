#include <algorithm>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <regex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "nodes/node_support.h"

namespace alg_framework {

/**
 * @brief 规则与关键词匹配通用算子 (TextRuleMatchNode)
 * 支持关键词包含、精确匹配与带命名捕获组的正则表达式匹配
 */
class TextRuleMatchNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "TextRuleMatchNode";

  struct RuleSpec {
    std::string id;
    std::string strategy;  // "contains", "exact", "regex"
    std::string pattern;
    std::string category;
    float score = 1.0f;
    std::unordered_map<std::string, std::string> constants;
    std::regex compiled_regex;
    std::vector<std::string> named_groups;
  };

  TextRuleMatchNode()
      : NodeBase(kNodeType), in_text_("text"), out_matches_("matches") {}

  NodeControlResult ControlNode(int cmd,
                                const std::string& json_param) override {
    if (cmd == kControlCmdUpdateRules) {  // 1: 动态更新关注词/规则表
      try {
        nlohmann::json root = nlohmann::json::parse(json_param);
        if (root.contains("categories") && root["categories"].is_object()) {
          UpdateCategories(root["categories"]);
          return NodeControlResult::Handled(
              0, "TextRuleMatchNode categories updated");
        } else if (root.contains("rules") && root["rules"].is_array()) {
          UpdateRules(root["rules"]);
          return NodeControlResult::Handled(0,
                                            "TextRuleMatchNode rules updated");
        }
      } catch (const std::exception& e) {
        return NodeControlResult::Failed(
            -1, std::string("JSON parse error in Control: ") + e.what());
      }
    }
    return NodeControlResult::Unsupported();
  }

 protected:
  bool InitNode(const NodeInitContext& init_ctx, const nlohmann::json& config,
                SessionContext& /*session_ctx*/) override {
    BindPort(init_ctx, in_text_);
    BindPort(init_ctx, out_matches_);

    if (config.contains("default_categories") &&
        config["default_categories"].is_object()) {
      UpdateCategories(config["default_categories"]);
    } else if (config.contains("categories") &&
               config["categories"].is_object()) {
      UpdateCategories(config["categories"]);
    }

    if (config.contains("rules") && config["rules"].is_array()) {
      UpdateRules(config["rules"]);
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
      std::string first_hit_rule_id;
      float first_hit_score = 0.0f;
      int is_hit = 0;
      std::unordered_map<std::string, std::string> total_captures;
      std::unordered_map<std::string, std::string> total_constants;
      nlohmann::json slots_obj = nlohmann::json::object();

      // 1. 匹配 categories (词表模式)
      for (const auto& [category, words] : category_keywords_list_) {
        for (const auto& w : words) {
          if (w.empty()) continue;
          if (sentence.find(w) != std::string::npos) {
            is_hit = 1;
            if (first_hit_category.empty()) {
              first_hit_category = category;
              first_hit_word = w;
              first_hit_score = 1.0f;
            }
            nlohmann::json match_elem;
            match_elem["category"] = category;
            match_elem["matched_word"] = w;
            matches_array.push_back(std::move(match_elem));
          }
        }
      }

      // 2. 匹配 rules (结构化规则模式，支持 regex, exact, contains)
      for (const auto& rule : rules_list_) {
        bool rule_matched = false;
        std::unordered_map<std::string, std::string> rule_captures;

        if (rule.strategy == "regex") {
          std::smatch m;
          if (std::regex_search(sentence, m, rule.compiled_regex)) {
            rule_matched = true;
            for (size_t g = 0; g < rule.named_groups.size() && g + 1 < m.size();
                 ++g) {
              const std::string& gname = rule.named_groups[g];
              if (!gname.empty()) {
                rule_captures[gname] = m[g + 1].str();
              }
            }
          }
        } else if (rule.strategy == "exact") {
          if (sentence == rule.pattern) {
            rule_matched = true;
          }
        } else {  // contains
          if (sentence.find(rule.pattern) != std::string::npos) {
            rule_matched = true;
          }
        }

        if (rule_matched) {
          is_hit = 1;
          if (first_hit_category.empty()) {
            first_hit_category = rule.category;
            first_hit_word = rule.pattern;
            first_hit_rule_id = rule.id;
            first_hit_score = rule.score;
          }
          for (const auto& [k, v] : rule_captures) {
            total_captures[k] = v;
            slots_obj[k] = v;
          }
          for (const auto& [k, v] : rule.constants) {
            total_constants[k] = v;
            slots_obj[k] = v;
          }

          nlohmann::json match_elem;
          match_elem["rule_id"] = rule.id;
          match_elem["category"] = rule.category;
          match_elem["pattern"] = rule.pattern;
          match_elem["score"] = rule.score;
          matches_array.push_back(std::move(match_elem));
        }
      }

      nlohmann::json result_json;
      result_json["matches"] = matches_array;
      if (is_hit && !first_hit_category.empty()) {
        result_json["intent"] = first_hit_category;
        result_json["matched_word"] = first_hit_word;
        result_json["confidence"] = first_hit_score;
        result_json["slots"] = slots_obj;
      }

      RuleMatchItem match_item(
          is_hit, first_hit_category, first_hit_word, result_json.dump(),
          is_hit ? first_hit_score : 0.0f, first_hit_rule_id);
      match_item.captures = std::move(total_captures);
      match_item.constants = std::move(total_constants);
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

  void UpdateRules(const nlohmann::json& rules_json) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    rules_list_.clear();
    for (const auto& r_elem : rules_json) {
      if (!r_elem.is_object()) continue;
      RuleSpec spec;
      spec.id = r_elem.value("id", "");
      spec.strategy = r_elem.value("strategy", "contains");
      spec.pattern = r_elem.value("pattern", "");
      spec.category = r_elem.value("category", "");
      spec.score = r_elem.value("score", 1.0f);

      if (r_elem.contains("constants") && r_elem["constants"].is_object()) {
        for (auto it = r_elem["constants"].begin();
             it != r_elem["constants"].end(); ++it) {
          if (it.value().is_string()) {
            spec.constants[it.key()] = it.value().get<std::string>();
          } else if (it.value().is_boolean()) {
            spec.constants[it.key()] =
                it.value().get<bool>() ? "true" : "false";
          } else if (it.value().is_number()) {
            spec.constants[it.key()] = it.value().dump();
          }
        }
      }

      if (spec.strategy == "regex" && !spec.pattern.empty()) {
        // 解析命名捕获组 (?<name>...) 或 (?P<name>...)
        std::string raw_pat = spec.pattern;
        std::string converted_pat;
        std::vector<std::string> group_names;

        size_t pos = 0;
        while (pos < raw_pat.size()) {
          if (raw_pat.substr(pos, 3) == "(?<" ||
              raw_pat.substr(pos, 4) == "(?P<") {
            size_t name_start = (raw_pat[pos + 2] == 'P') ? pos + 4 : pos + 3;
            size_t name_end = raw_pat.find('>', name_start);
            if (name_end != std::string::npos) {
              std::string gname =
                  raw_pat.substr(name_start, name_end - name_start);
              group_names.push_back(std::move(gname));
              converted_pat += "(";
              pos = name_end + 1;
              continue;
            }
          }
          converted_pat += raw_pat[pos];
          pos++;
        }

        spec.named_groups = std::move(group_names);
        try {
          spec.compiled_regex = std::regex(converted_pat);
        } catch (...) {
          std::cerr << "[TextRuleMatchNode] Invalid regex: " << spec.pattern
                    << std::endl;
        }
      }
      rules_list_.push_back(std::move(spec));
    }
  }

  mutable std::shared_mutex rw_mutex_;
  std::vector<std::pair<std::string, std::vector<std::string>>>
      category_keywords_list_;
  std::vector<RuleSpec> rules_list_;

  BoundInput<TextBatch> in_text_;
  BoundOutput<RuleMatchBatch> out_matches_;
};

NodeDefinition MakeTextRuleMatchNodeDefinition() {
  NodeDefinition def;
  def.node_type = TextRuleMatchNode::kNodeType;
  def.category = "common";
  def.description =
      "Keyword and rule matching engine node with regex slot capture";
  def.inputs = {
      RequiredInputPort("text", BlackboardKey<TextBatch>{"", "TextBatch"})};
  def.outputs = {OutputPort(
      "matches", BlackboardKey<RuleMatchBatch>{"", "RuleMatchBatch"})};
  def.control_commands = {ControlCommandDefinition(
      kControlCmdUpdateRules, "update_rules",
      "Update matching rules and categories dynamically",
      nlohmann::json::object(), true)};
  def.config_fields = {
      ConfigFieldDefinition{"default_categories", ConfigValueKind::kObject,
                            false},
      ConfigFieldDefinition{"categories", ConfigValueKind::kObject, false},
      ConfigFieldDefinition{"rules", ConfigValueKind::kArray, false}};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(TextRuleMatchNode,
                              MakeTextRuleMatchNodeDefinition());

}  // namespace alg_framework

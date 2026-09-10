#include <algorithm>
#include <cmath>
#include <mutex>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common_nodes/support/compiled_text_regex.h"
#include "contracts/config_schema_validation.h"
#include "contracts/control_payload.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "edgeflow/log.h"
#include "nodes/node_base.h"
#include "nodes/node_error_codes.h"

namespace llm_edgeflow {
namespace {
constexpr double kDefaultScore = 1.0;

const std::vector<ConfigFieldDefinition>& TextRuleMatchConfigFields() {
  static const std::vector<ConfigFieldDefinition> kFields = {
      ConfigFieldDefinition{"default_category",
                            ConfigValueKind::kString,
                            false,
                            "",
                            std::nullopt,
                            std::nullopt,
                            {},
                            "没有词表或规则命中时使用的类别；非空会将该输入标记"
                            "为命中，并保留 raw_query。"},
      ConfigFieldDefinition{"default_score",
                            ConfigValueKind::kNumber,
                            false,
                            kDefaultScore,
                            0.0,
                            1.0,
                            {},
                            "仅 default_category 回退命中时使用的分数，范围 "
                            "[0,1]；规则自身分数由 rules[].score 设置。"},
      ConfigFieldDefinition{"categories",
                            ConfigValueKind::kObject,
                            false,
                            nlohmann::json(),
                            std::nullopt,
                            std::nullopt,
                            {},
                            "类别到关键词数组的映射，按子串匹配，例如 "
                            "{\"VIP\":[\"专席\",\"VIP\"]}；可通过 update_rules "
                            "Control 整体替换。"},
      ConfigFieldDefinition{
          "rules",
          ConfigValueKind::kArray,
          false,
          nlohmann::json(),
          std::nullopt,
          std::nullopt,
          {},
          "规则对象数组，pattern 必填；例如 "
          "[{\"id\":\"r1\",\"strategy\":\"contains\",\"pattern\":\"VIP\","
          "\"category\":\"优先\",\"score\":1,\"constants\":{\"route\":\"vip\"}}"
          "]。strategy 还支持 exact/regex，score 范围 [0,1]。"}};
  return kFields;
}

const nlohmann::json& RuleItemSchema() {
  static const nlohmann::json schema = {
      {"type", "object"},
      {"required", {"pattern"}},
      {"additionalProperties", false},
      {"properties",
       {{"id", {{"type", "string"}}},
        {"strategy",
         {{"type", "string"}, {"enum", {"contains", "exact", "regex"}}}},
        {"pattern", {{"type", "string"}}},
        {"category", {{"type", "string"}}},
        {"score", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
        {"constants", {{"type", "object"}}}}}};
  return schema;
}
const nlohmann::json& RuleControlSchema() {
  static const nlohmann::json schema = {
      {"type", "object"},
      {"minProperties", 1},
      {"additionalProperties", false},
      {"properties",
       {{"categories",
         {{"type", "object"},
          {"additionalProperties",
           {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
        {"rules", {{"type", "array"}, {"items", RuleItemSchema()}}}}}};
  return schema;
}
}  // namespace

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
    float score = kDefaultScore;
    std::unordered_map<std::string, std::string> constants;
    std::unordered_map<std::string, nlohmann::json> constants_json;
    CompiledTextRegex compiled_regex;
  };

  TextRuleMatchNode()
      : NodeBase(kNodeType), in_text_("text"), out_matches_("matches") {}

  NodeControlResult ControlNode(int cmd,
                                const std::string& json_param) override {
    if (cmd != kControlCmdUpdateRules) return NodeControlResult::Unsupported();
    nlohmann::json root;
    std::string error;
    if (!ParseControlPayload(json_param, RuleControlSchema(), &root, &error)) {
      return NodeControlResult::Failed(node_error::control::kInvalidRequest,
                                       error);
    }
    const bool has_categories = root.contains("categories");
    const bool has_rules = root.contains("rules");
    CategoryList new_categories;
    std::vector<RuleSpec> new_rules;
    if (has_categories &&
        !BuildCategories(root["categories"], &new_categories, &error)) {
      return NodeControlResult::Failed(node_error::control::kInvalidRequest,
                                       error);
    }
    if (has_rules && !BuildRules(root["rules"], &new_rules, &error)) {
      return NodeControlResult::Failed(node_error::control::kInvalidRequest,
                                       error);
    }
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    if (has_categories) category_keywords_list_ = std::move(new_categories);
    if (has_rules) rules_list_ = std::move(new_rules);
    return NodeControlResult::Handled();
  }

  static bool ValidateConfig(const nlohmann::json& config,
                             const std::unordered_set<std::string>&,
                             std::string* diagnostic) {
    nlohmann::json normalized;
    CategoryList categories;
    std::vector<RuleSpec> rules;
    return ParseConfig(config, &normalized, &categories, &rules, diagnostic);
  }

 protected:
  bool InitNode(const NodeInitContext& init_ctx, const nlohmann::json& config,
                SessionContext& /*session_ctx*/) override {
    nlohmann::json normalized;
    CategoryList categories;
    std::vector<RuleSpec> rules;
    std::string diagnostic;
    if (!ParseConfig(config, &normalized, &categories, &rules, &diagnostic))
      return init_ctx.Fail(diagnostic);

    BindPort(init_ctx, in_text_);
    BindPort(init_ctx, out_matches_);
    default_category_ = normalized["default_category"].get<std::string>();
    default_score_ = normalized["default_score"].get<float>();
    category_keywords_list_ = std::move(categories);
    rules_list_ = std::move(rules);
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* text_items =
        in_text_.Require(req_ctx, node_error::text_rule_match::kMissingInput,
                         "TextRuleMatchNode input");
    if (!text_items) {
      return node_error::text_rule_match::kMissingInput;
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
          std::string diagnostic;
          const TextRegexSearchStatus status =
              rule.compiled_regex.Search(sentence, &rule_captures, &diagnostic);
          if (status == TextRegexSearchStatus::kError) {
            ALG_LOG_ERROR(
                "[TextRuleMatchNode] Regex execution failed for rule '%s': "
                "%s\n",
                rule.id.c_str(), diagnostic.c_str());
            return Fail(req_ctx,
                        node_error::text_rule_match::kRegexExecutionFailed,
                        "Regex execution failed for rule '" + rule.id +
                            "': " + diagnostic);
          }
          rule_matched = status == TextRegexSearchStatus::kMatched;
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
          for (const auto& [k, v] : rule.constants_json) {
            slots_obj[k] = v;
          }
          for (const auto& [k, v] : rule.constants) {
            total_constants[k] = v;
          }

          nlohmann::json match_elem;
          match_elem["rule_id"] = rule.id;
          match_elem["category"] = rule.category;
          match_elem["pattern"] = rule.pattern;
          match_elem["score"] = rule.score;
          matches_array.push_back(std::move(match_elem));
        }
      }

      if (!is_hit && !default_category_.empty()) {
        is_hit = 1;
        first_hit_category = default_category_;
        first_hit_score = default_score_;
        first_hit_word = "";
        slots_obj["raw_query"] = sentence;
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
  using CategoryList =
      std::vector<std::pair<std::string, std::vector<std::string>>>;

  static bool BuildCategories(const nlohmann::json& categories_json,
                              CategoryList* out_categories,
                              std::string* diagnostic) {
    std::string detail;
    if (!ValidateControlPayload(categories_json,
                                RuleControlSchema()["properties"]["categories"],
                                &detail)) {
      if (diagnostic) *diagnostic = "categories: " + detail;
      return false;
    }
    CategoryList temp_categories;
    for (auto it = categories_json.begin(); it != categories_json.end(); ++it) {
      std::vector<std::string> words;
      for (const auto& w : it.value()) {
        words.push_back(w.get<std::string>());
      }
      temp_categories.push_back({it.key(), std::move(words)});
    }
    *out_categories = std::move(temp_categories);
    return true;
  }

  static bool BuildRules(const nlohmann::json& rules_json,
                         std::vector<RuleSpec>* out_rules,
                         std::string* diagnostic) {
    std::string detail;
    if (!ValidateControlPayload(
            rules_json, RuleControlSchema()["properties"]["rules"], &detail)) {
      if (diagnostic) *diagnostic = "rules: " + detail;
      return false;
    }
    std::vector<RuleSpec> temp_rules;
    for (size_t index = 0; index < rules_json.size(); ++index) {
      const auto& r_elem = rules_json[index];
      RuleSpec spec;
      spec.id = r_elem.value("id", "");
      spec.strategy = r_elem.value("strategy", "contains");
      spec.pattern = r_elem.value("pattern", "");
      spec.category = r_elem.value("category", "");
      spec.score = r_elem.value("score", static_cast<float>(kDefaultScore));

      if (r_elem.contains("constants") && r_elem["constants"].is_object()) {
        for (auto it = r_elem["constants"].begin();
             it != r_elem["constants"].end(); ++it) {
          spec.constants_json[it.key()] = it.value();
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
        if (!spec.compiled_regex.Compile(spec.pattern, &detail)) {
          if (diagnostic) {
            *diagnostic = "rules[" + std::to_string(index) + "].pattern" +
                          (spec.id.empty() ? "" : " (id='" + spec.id + "')") +
                          ": Invalid regex: " + detail;
          }
          return false;
        }
      }
      temp_rules.push_back(std::move(spec));
    }
    *out_rules = std::move(temp_rules);
    return true;
  }

  static bool ParseConfig(const nlohmann::json& config,
                          nlohmann::json* normalized, CategoryList* categories,
                          std::vector<RuleSpec>* rules,
                          std::string* diagnostic) {
    if (diagnostic) diagnostic->clear();
    std::vector<ConfigFieldValidationError> errors;
    if (!ValidateAndNormalizeFields(TextRuleMatchConfigFields(), config,
                                    normalized, &errors)) {
      if (diagnostic) {
        const auto& error = errors.front();
        *diagnostic = error.field_name.empty()
                          ? error.message
                          : error.field_name + ": " + error.message;
      }
      return false;
    }
    return (!normalized->contains("categories") ||
            BuildCategories((*normalized)["categories"], categories,
                            diagnostic)) &&
           (!normalized->contains("rules") ||
            BuildRules((*normalized)["rules"], rules, diagnostic));
  }

  mutable std::shared_mutex rw_mutex_;
  std::vector<std::pair<std::string, std::vector<std::string>>>
      category_keywords_list_;
  std::vector<RuleSpec> rules_list_;
  std::string default_category_;
  float default_score_ = kDefaultScore;

  BoundInput<TextBatch> in_text_;
  BoundOutput<RuleMatchBatch> out_matches_;
};

NodeDefinition MakeTextRuleMatchNodeDefinition() {
  NodeDefinition def;
  def.node_type = TextRuleMatchNode::kNodeType;
  def.category = "common";
  def.validate_config = TextRuleMatchNode::ValidateConfig;
  def.description =
      "Keyword and Unicode regex matching with lookbehind and named captures";
  def.inputs = {RequiredInputPort("text",
                                  BlackboardKey<TextBatch>{"", "TextBatch"},
                                  "1:1", "preserve", "request")};
  def.outputs = {OutputPort("matches",
                            BlackboardKey<RuleMatchBatch>{"", "RuleMatchBatch"},
                            "1:1", "preserve", "request")};
  def.control_commands = {ControlCommandDefinition(
      kControlCmdUpdateRules, "update_rules",
      "Update matching rules and categories dynamically", RuleControlSchema(),
      true)};
  def.control_commands.front().shared_id = true;
  def.config_fields = TextRuleMatchConfigFields();
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(TextRuleMatchNode,
                              MakeTextRuleMatchNodeDefinition());

}  // namespace llm_edgeflow

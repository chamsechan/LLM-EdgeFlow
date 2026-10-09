#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common_nodes/support/compiled_text_regex.h"
#include "core/common_contracts.h"
#include "edgeflow/log.h"
#include "nodes/authoring.h"
#include "nodes/node_error_codes.h"

namespace llm_edgeflow {
namespace {
struct RuleSpec {
  std::string id;
  std::string strategy;  // "contains", "exact", "regex"
  std::string pattern;
  std::string category;
  float score = 1.0f;
  std::map<std::string, nlohmann::json> constants;
  std::shared_ptr<const CompiledTextRegex> compiled_regex;
};

Parameters<RuleSpec> RuleParameters() {
  auto parameters = Parameters<RuleSpec>(
      {Field("pattern", &RuleSpec::pattern).Required(),
       Field("id", &RuleSpec::id).Default(""),
       Field("strategy", &RuleSpec::strategy)
           .Default("contains")
           .Enum({"contains", "exact", "regex"}),
       Field("category", &RuleSpec::category).Default(""),
       Field("score", &RuleSpec::score).Default(1.0f).Range(0, 1),
       Field("constants", &RuleSpec::constants).Default({})});
  parameters.Prepare([](RuleSpec* rule, std::string* diagnostic) {
    rule->compiled_regex.reset();
    if (rule->strategy != "regex" || rule->pattern.empty()) return true;
    auto compiled = std::make_shared<CompiledTextRegex>();
    std::string detail;
    if (!compiled->Compile(rule->pattern, &detail)) {
      if (diagnostic) {
        *diagnostic =
            "pattern (id='" + rule->id + "'): Invalid regex: " + detail;
      }
      return false;
    }
    rule->compiled_regex = std::move(compiled);
    return true;
  });
  return parameters;
}

struct Params {
  std::map<std::string, std::vector<std::string>> categories;
  std::vector<RuleSpec> rules;
  std::string default_category;
  float default_score = 1.0f;
};

Parameters<Params> ParamSpec() {
  return Parameters<Params>(
      {Field("default_category", &Params::default_category)
           .Default("")
           .Description("没有词表或规则命中时使用的类别；非空会将该输入标记"
                        "为命中，并保留 raw_query。"),
       Field("default_score", &Params::default_score)
           .Default(1.0f)
           .Range(0, 1)
           .Description("仅 default_category 回退命中时使用的分数，范围 "
                        "[0,1]；规则自身分数由 rules[].score 设置。"),
       Field("categories", &Params::categories)
           .Default({})
           .Description("类别到关键词数组的映射，按子串匹配，例如 "
                        "{\"VIP\":[\"专席\",\"VIP\"]}；可通过 update_rules "
                        "Control 整体替换。"),
       Field("rules", &Params::rules)
           .Default({})
           .Items(RuleParameters())
           .Description(
               "规则对象数组，pattern 必填；例如 "
               "[{\"id\":\"r1\",\"strategy\":\"contains\",\"pattern\":\"VIP\","
               "\"category\":\"优先\",\"score\":1,\"constants\":{\"route\":"
               "\"vip\"}}"
               "]。strategy 还支持 exact/regex，score 范围 [0,1]。")});
}

struct Inputs {
  const TextBatch* text = nullptr;
};

NodeResult<RuleMatchBatch> Run(const Inputs& inputs, const Params& state) {
  const auto* text_items = inputs.text;
  RuleMatchBatch output_matches;
  output_matches.reserve(text_items->size());

  for (const auto& item : *text_items) {
    const std::string& sentence = item.data;
    RuleMatchItem result;
    // 首个带类别的命中决定结果名称。
    auto record_hit = [&](const std::string& category, const std::string& word,
                          float score, const std::string& rule_id) {
      result.is_hit = 1;
      if (!result.category.empty()) return;
      result.category = category;
      result.matched_word = word;
      result.score = score;
      result.rule_id = rule_id;
    };

    // 1. 匹配 categories (词表模式)
    for (const auto& [category, words] : state.categories) {
      for (const auto& w : words) {
        if (w.empty()) continue;
        if (sentence.find(w) != std::string::npos) {
          record_hit(category, w, 1.0f, {});
          result.matches.push_back(
              {RuleMatchSource::kKeyword, category, {}, w, 1.0f});
        }
      }
    }

    // 2. 匹配 rules (结构化规则模式，支持 regex, exact, contains)
    for (const auto& rule : state.rules) {
      bool rule_matched = false;
      std::unordered_map<std::string, std::string> rule_captures;

      if (rule.strategy == "regex") {
        std::string diagnostic;
        const TextRegexSearchStatus status =
            rule.compiled_regex ? rule.compiled_regex->Search(
                                      sentence, &rule_captures, &diagnostic)
                                : TextRegexSearchStatus::kNotMatched;
        if (status == TextRegexSearchStatus::kError) {
          ALG_LOG_ERROR(
              "[text_rule_match] Regex execution failed for rule '%s': "
              "%s\n",
              rule.id.c_str(), diagnostic.c_str());
          return NodeResult<RuleMatchBatch>::Failure(
              NodeErrorKind::kBusinessError,
              "Regex execution failed for rule '" + rule.id +
                  "': " + diagnostic,
              node_error::text_rule_match::kRegexExecutionFailed);
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
        record_hit(rule.category, rule.pattern, rule.score, rule.id);
        for (const auto& [k, v] : rule_captures) result.slots[k] = v;
        for (const auto& [k, v] : rule.constants) result.slots[k] = v;
        result.matches.push_back({RuleMatchSource::kRule, rule.category,
                                  rule.id, rule.pattern, rule.score});
      }
    }

    if (!result.is_hit && !state.default_category.empty()) {
      record_hit(state.default_category, "", state.default_score, {});
      result.slots["raw_query"] = sentence;
    }

    output_matches.emplace_back(item.req_id, item.sub_id, std::move(result));
  }

  return NodeResult<RuleMatchBatch>::Success(std::move(output_matches));
}

auto Spec() {
  return MakeNodeSpec(InputsOf<Inputs>{Required("text", &Inputs::text)},
                      PreservedOutput<RuleMatchBatch>("matches", "text"),
                      ParamSpec(), Run)
      .Category("common")
      .Description(
          "Keyword and Unicode regex matching with lookbehind and named "
          "captures")
      .ParallelSafe(true)
      .WithControls({ReplaceFields(kControlCmdUpdateRules, "update_rules",
                                   {"categories", "rules"})});
}
}  // namespace

REGISTER_FUNCTION_NODE(text_rule_match, Spec());

}  // namespace llm_edgeflow

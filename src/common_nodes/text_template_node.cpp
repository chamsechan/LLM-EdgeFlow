#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/common_contracts.h"
#include "engine/text/utf8.h"
#include "nodes/authoring.h"
#include "nodes/node_error_codes.h"
#include "nodes/text_template.h"

namespace llm_edgeflow {
namespace {
constexpr char kDefaultTemplate[] = "{{primary}}";
constexpr char kDefaultSeparator[] = "\n";
constexpr int64_t kDefaultMaxLength = 65536;
constexpr char kDefaultMissingPolicy[] = "fail";

const std::vector<ConfigFieldDefinition>& TextTemplateConfigFields() {
  static const std::vector<ConfigFieldDefinition> fields = {
      ConfigFieldDefinition{
          "template",
          ConfigValueKind::kString,
          false,
          kDefaultTemplate,
          std::nullopt,
          std::nullopt,
          {},
          "文本模板，使用 {{primary}}/{{context}} 等变量；例如 "
          "\"问题：{{primary}}\"，默认缺失策略要求连接变量对应的输入。"},
      ConfigFieldDefinition{
          "separator",
          ConfigValueKind::kString,
          false,
          kDefaultSeparator,
          std::nullopt,
          std::nullopt,
          {},
          "同一请求聚合多个输入文本时使用的分隔字符串，例如换行。"},
      ConfigFieldDefinition{
          "max_length",
          ConfigValueKind::kInteger,
          false,
          kDefaultMaxLength,
          1.0,
          1048576.0,
          {},
          "每条渲染结果的 UTF-8 字节上限；超出后按 overflow_policy 处理。"},
      ConfigFieldDefinition{
          "allow_dynamic_attributes",
          ConfigValueKind::kBoolean,
          false,
          false,
          std::nullopt,
          std::nullopt,
          {},
          "允许静态 values 之外的自定义变量；连接 attributes "
          "输入时自动允许，缺失值按 missing_variable_policy 处理。"},
      ConfigFieldDefinition{"overflow_policy",
                            ConfigValueKind::kString,
                            false,
                            "fail",
                            std::nullopt,
                            std::nullopt,
                            {"fail", "truncate"},
                            "fail 拒绝超过 max_length 的结果；truncate 按 "
                            "UTF-8 字符边界截断到字节上限内。"},
      ConfigFieldDefinition{
          "missing_variable_policy",
          ConfigValueKind::kString,
          false,
          kDefaultMissingPolicy,
          std::nullopt,
          std::nullopt,
          {"fail", "empty", "preserve"},
          "变量缺失时：fail 报错；empty 替换为空字符串；preserve 保留占位符。"},
      ConfigFieldDefinition{
          "values",
          ConfigValueKind::kObject,
          false,
          nlohmann::json(),
          std::nullopt,
          std::nullopt,
          {},
          "静态变量名到字符串的映射，例如 {\"role\":\"客服\"}，可在模板中用 "
          "{{role}} 引用。"}};
  return fields;
}

const std::unordered_map<std::string, std::vector<std::string>>&
BuiltinInputs() {
  static const std::unordered_map<std::string, std::vector<std::string>>
      inputs = {{"primary", {"primary"}},
                {"context", {"context", "context_text"}},
                {"context_text", {"context", "context_text"}},
                {"matches", {"matches"}},
                {"document", {"document", "document_text"}},
                {"document_text", {"document", "document_text"}}};
  return inputs;
}

bool ValidateTemplateInputs(const std::vector<TextTemplateToken>& tokens,
                            const std::unordered_set<std::string>& connected,
                            const std::string& missing_policy,
                            std::string* diagnostic = nullptr) {
  if (missing_policy != "fail") return true;
  for (const auto& token : tokens) {
    if (token.type != TextTemplateTokenType::kVariable) continue;
    const auto ports = BuiltinInputs().find(token.value);
    if (ports == BuiltinInputs().end()) continue;
    if (std::none_of(ports->second.begin(), ports->second.end(),
                     [&](const auto& port) { return connected.count(port); })) {
      if (diagnostic)
        *diagnostic = "Template variable '" + token.value +
                      "' requires a connected input or a non-failing "
                      "missing_variable_policy";
      return false;
    }
  }
  return true;
}

const nlohmann::json& TemplateControlSchema() {
  static const nlohmann::json schema = nlohmann::json{
      {"type", "object"},
      {"minProperties", 1},
      {"additionalProperties", false},
      {"properties",
       {{"template", {{"type", "string"}}},
        {"prompt_id", {{"type", "string"}}},
        {"values",
         {{"type", "object"}, {"additionalProperties", {{"type", "string"}}}}},
        {"allow_dynamic_attributes", {{"type", "boolean"}}},
        {"missing_variable_policy",
         {{"type", "string"}, {"enum", {"fail", "empty", "preserve"}}}}}}};
  return schema;
}
struct TemplateState {
  std::string template_str = kDefaultTemplate;
  std::string separator = kDefaultSeparator;
  size_t max_length = kDefaultMaxLength;
  std::string overflow_policy = "fail";
  std::string missing_variable_policy = kDefaultMissingPolicy;
  std::string prompt_id;
  bool allow_dynamic_attrs = false;
  std::unordered_map<std::string, std::string> static_values;
  std::vector<TextTemplateToken> compiled_tokens;
};

struct TemplateUpdate {
  std::optional<std::string> template_str;
  std::optional<std::string> prompt_id;
  std::optional<std::unordered_map<std::string, std::string>> values;
  std::optional<bool> allow_dynamic_attributes;
  std::optional<std::string> missing_variable_policy;
};

inline bool CompileTemplate(
    const std::string& tmpl,
    const std::unordered_map<std::string, std::string>& static_vals,
    bool allow_dynamic_attrs, std::vector<TextTemplateToken>* out_tokens,
    std::string* diagnostic = nullptr) {
  std::string error;
  bool ok = ParseTextTemplate(tmpl, out_tokens, &error);
  if (ok) {
    for (const auto& token : *out_tokens) {
      if (token.type == TextTemplateTokenType::kVariable &&
          !allow_dynamic_attrs && !BuiltinInputs().count(token.value) &&
          !static_vals.count(token.value)) {
        error = "Unknown template placeholder: " + token.value +
                "; connect attributes or declare values";
        ok = false;
        break;
      }
    }
  }
  if (!ok) {
    out_tokens->clear();
    ALG_LOG_ERROR("[TextTemplateNode] %s\n", error.c_str());
    if (diagnostic) *diagnostic = std::move(error);
  }
  return ok;
}

bool BuildTemplateState(TemplateState* state,
                        const std::unordered_set<std::string>* connected_inputs,
                        std::string* diagnostic) {
  std::vector<TextTemplateToken> tokens;
  if (!CompileTemplate(state->template_str, state->static_values,
                       state->allow_dynamic_attrs, &tokens, diagnostic)) {
    return false;
  }
  if (connected_inputs &&
      !ValidateTemplateInputs(tokens, *connected_inputs,
                              state->missing_variable_policy, diagnostic)) {
    return false;
  }
  state->compiled_tokens = std::move(tokens);
  return true;
}

inline NodeResult<TemplateState> BuildNextTemplate(
    const TemplateState& current, const TemplateUpdate& update,
    const BindingFacts& bindings) {
  TemplateState next = current;
  if (update.template_str) next.template_str = *update.template_str;
  if (update.prompt_id) next.prompt_id = *update.prompt_id;
  if (update.missing_variable_policy) {
    next.missing_variable_policy = *update.missing_variable_policy;
  }
  if (update.allow_dynamic_attributes) {
    next.allow_dynamic_attrs =
        bindings.IsConnected("attributes") || *update.allow_dynamic_attributes;
  }
  if (update.values) {
    for (const auto& [k, v] : *update.values) {
      next.static_values[k] = v;
    }
  }
  std::string diagnostic;
  if (!BuildTemplateState(
          &next, bindings.has_bindings ? &bindings.connected_inputs : nullptr,
          &diagnostic)) {
    return NodeResult<TemplateState>::Failure(
        NodeErrorKind::kBusinessError,
        diagnostic.empty()
            ? "Invalid template placeholders or syntax in Control"
            : diagnostic,
        node_error::control::kInvalidRequest);
  }
  return NodeResult<TemplateState>::Success(std::move(next));
}

struct TemplateInputs {
  const TextBatch* primary = nullptr;
  const RankedTextBatch* context = nullptr;
  const TextBatch* context_text = nullptr;
  const RuleMatchBatch* matches = nullptr;
  const OcrDocumentBatch* document = nullptr;
  const TextBatch* document_text = nullptr;
  const TextAttributesBatch* attributes = nullptr;
};

NodeResult<TextBatch> RenderTemplate(const TemplateInputs& inputs,
                                     const TemplateState& state) {
  const auto* primary_items = inputs.primary;
  const auto* context_items = inputs.context;
  const auto* context_text_items = inputs.context_text;
  const auto* matches_items = inputs.matches;
  const auto* document_items = inputs.document;
  const auto* document_text_items = inputs.document_text;
  const auto* attributes_items = inputs.attributes;
  // 收集所有请求-子项样本键值 (req_id, sub_id)
  struct SampleKey {
    uint32_t req_id;
    uint32_t sub_id;
    bool operator==(const SampleKey& o) const {
      return req_id == o.req_id && sub_id == o.sub_id;
    }
  };
  std::vector<SampleKey> ordered_samples;
  auto record_sample = [&](uint32_t r, uint32_t s) {
    SampleKey k{r, s};
    if (std::find(ordered_samples.begin(), ordered_samples.end(), k) ==
        ordered_samples.end()) {
      ordered_samples.push_back(k);
    }
  };

  std::map<std::pair<uint32_t, uint32_t>, std::string> primary_by_sample;
  std::unordered_map<uint32_t, std::vector<std::string>> context_by_req;
  std::unordered_map<uint32_t, std::vector<std::string>> matches_by_req;
  std::unordered_map<uint32_t, std::vector<std::string>> document_by_req;
  std::map<std::pair<uint32_t, uint32_t>,
           std::unordered_map<std::string, std::string>>
      attributes_by_sample;
  std::set<uint32_t> req_ids_from_aggregated;

  if (primary_items) {
    for (const auto& item : *primary_items) {
      record_sample(item.req_id, item.sub_id);
      primary_by_sample[{item.req_id, item.sub_id}] = item.data;
    }
  }

  if (attributes_items) {
    for (const auto& item : *attributes_items) {
      record_sample(item.req_id, item.sub_id);
      attributes_by_sample[{item.req_id, item.sub_id}] = item.data;
    }
  }

  if (context_items) {
    for (const auto& item : *context_items) {
      req_ids_from_aggregated.insert(item.req_id);
      context_by_req[item.req_id].push_back(item.data.text);
    }
  }

  if (context_text_items) {
    for (const auto& item : *context_text_items) {
      req_ids_from_aggregated.insert(item.req_id);
      context_by_req[item.req_id].push_back(item.data);
    }
  }

  if (matches_items) {
    for (const auto& item : *matches_items) {
      req_ids_from_aggregated.insert(item.req_id);
      std::string match_repr;
      if (!item.data.category.empty() && !item.data.matched_word.empty()) {
        match_repr = item.data.category + " (" + item.data.matched_word + ")";
      } else if (!item.data.category.empty()) {
        match_repr = item.data.category;
      } else if (!item.data.matched_word.empty()) {
        match_repr = item.data.matched_word;
      }
      if (!match_repr.empty()) {
        matches_by_req[item.req_id].push_back(std::move(match_repr));
      }
    }
  }

  if (document_items) {
    for (const auto& item : *document_items) {
      req_ids_from_aggregated.insert(item.req_id);
      document_by_req[item.req_id].push_back(item.data.combined_text);
    }
  }

  if (document_text_items) {
    for (const auto& item : *document_text_items) {
      req_ids_from_aggregated.insert(item.req_id);
      document_by_req[item.req_id].push_back(item.data);
    }
  }

  if (ordered_samples.empty()) {
    for (uint32_t r : req_ids_from_aggregated) {
      record_sample(r, 0);
    }
  }

  TextBatch output_batch;
  output_batch.reserve(ordered_samples.size());

  for (const auto& sample : ordered_samples) {
    uint32_t req_id = sample.req_id;
    uint32_t sub_id = sample.sub_id;

    std::string primary_str;
    auto p_it = primary_by_sample.find({req_id, sub_id});
    if (p_it != primary_by_sample.end()) {
      primary_str = p_it->second;
    }

    std::string context_str;
    auto c_it = context_by_req.find(req_id);
    if (c_it != context_by_req.end()) {
      for (size_t i = 0; i < c_it->second.size(); ++i) {
        if (i > 0) context_str += state.separator;
        context_str += c_it->second[i];
      }
    }

    std::string matches_str;
    auto m_it = matches_by_req.find(req_id);
    if (m_it != matches_by_req.end()) {
      for (size_t i = 0; i < m_it->second.size(); ++i) {
        if (i > 0) matches_str += ", ";
        matches_str += m_it->second[i];
      }
    }

    std::string doc_str;
    auto d_it = document_by_req.find(req_id);
    if (d_it != document_by_req.end()) {
      for (size_t i = 0; i < d_it->second.size(); ++i) {
        if (i > 0) doc_str += state.separator;
        doc_str += d_it->second[i];
      }
    }

    const std::unordered_map<std::string, std::string>* attrs_ptr = nullptr;
    auto a_it = attributes_by_sample.find({req_id, sub_id});
    if (a_it != attributes_by_sample.end()) {
      attrs_ptr = &a_it->second;
    }

    std::string rendered;
    rendered.reserve(256);

    for (const auto& token : state.compiled_tokens) {
      if (token.type == TextTemplateTokenType::kLiteral) {
        rendered += token.value;
      } else {
        const std::string& var = token.value;
        const std::string* value = nullptr;
        if (var == "primary") {
          if (p_it != primary_by_sample.end()) value = &primary_str;
        } else if (var == "context" || var == "context_text") {
          // A present aggregate batch may contain no results for this
          // request.
          if (context_items || context_text_items) value = &context_str;
        } else if (var == "matches") {
          if (matches_items) value = &matches_str;
        } else if (var == "document" || var == "document_text") {
          if (document_items || document_text_items) value = &doc_str;
        } else if (attrs_ptr && attrs_ptr->find(var) != attrs_ptr->end()) {
          value = &attrs_ptr->at(var);
        } else if (state.static_values.find(var) != state.static_values.end()) {
          value = &state.static_values.at(var);
        }
        if (value) {
          rendered += *value;
        } else {
          if (state.missing_variable_policy == "fail") {
            return NodeResult<TextBatch>::Failure(
                NodeErrorKind::kBusinessError,
                "Missing required template variable: " + var,
                node_error::text_template::kMissingVariable);
          } else if (state.missing_variable_policy == "preserve") {
            rendered += "{{" + var + "}}";
          }
        }
      }
    }

    if (rendered.size() > state.max_length) {
      if (state.overflow_policy == "fail") {
        return NodeResult<TextBatch>::Failure(
            NodeErrorKind::kBusinessError,
            "Rendered prompt exceeds max_length of " +
                std::to_string(state.max_length),
            node_error::text_template::kRenderedOutputTooLong);
      }
      std::vector<size_t> boundaries;
      size_t invalid_offset = 0;
      if (!utf8::BuildCodePointBoundaries(rendered, &boundaries,
                                          &invalid_offset)) {
        return NodeResult<TextBatch>::Failure(
            NodeErrorKind::kBusinessError,
            "Rendered prompt contains invalid UTF-8 at byte offset " +
                std::to_string(invalid_offset),
            node_error::text_template::kInvalidUtf8);
      }
      const auto boundary = std::upper_bound(
          boundaries.begin(), boundaries.end(), state.max_length);
      rendered.resize(*(boundary - 1));
    }

    output_batch.emplace_back(req_id, sub_id, std::move(rendered));
  }

  return NodeResult<TextBatch>::Success(std::move(output_batch));
}

NodeResult<TemplateState> UpdateTemplate(const TemplateState& current,
                                         const nlohmann::json& root,
                                         const BindingFacts& bindings) {
  TemplateUpdate update;
  if (root.contains("template"))
    update.template_str = root["template"].get<std::string>();
  if (root.contains("allow_dynamic_attributes")) {
    update.allow_dynamic_attributes =
        root["allow_dynamic_attributes"].get<bool>();
  }
  if (root.contains("missing_variable_policy")) {
    update.missing_variable_policy =
        root["missing_variable_policy"].get<std::string>();
  }
  if (root.contains("prompt_id"))
    update.prompt_id = root["prompt_id"].get<std::string>();
  if (root.contains("values")) {
    std::unordered_map<std::string, std::string> vals;
    for (auto it = root["values"].begin(); it != root["values"].end(); ++it) {
      vals[it.key()] = it.value().get<std::string>();
    }
    update.values = std::move(vals);
  }
  return BuildNextTemplate(current, update, bindings);
}

auto MakeTextTemplateSpec() {
  auto parameters =
      Parameters<TemplateState>{}
          .WithParser(NodeConfigParser<TemplateState>(
              TextTemplateConfigFields(),
              [](const nlohmann::json& config, TemplateState* state,
                 std::string*) {
                state->template_str = config.at("template").get<std::string>();
                state->separator = config.at("separator").get<std::string>();
                state->max_length = config.at("max_length").get<size_t>();
                state->overflow_policy =
                    config.at("overflow_policy").get<std::string>();
                state->missing_variable_policy =
                    config.at("missing_variable_policy").get<std::string>();
                state->allow_dynamic_attrs =
                    config.at("allow_dynamic_attributes").get<bool>();
                if (config.contains("values"))
                  state->static_values =
                      config.at("values").get<decltype(state->static_values)>();
                return true;
              }))
          .Prepare([](TemplateState* state, const BindingFacts& bindings,
                      std::string* diagnostic) {
            state->allow_dynamic_attrs = state->allow_dynamic_attrs ||
                                         bindings.IsConnected("attributes");
            return BuildTemplateState(state, &bindings.connected_inputs,
                                      diagnostic);
          });
  auto control = ControlCommandDefinition(
      kControlCmdUpdatePrompt, "update_prompt",
      "Update template string dynamically", TemplateControlSchema(), true);
  control.shared_id = true;
  return MakeBatchSpec(
             InputsOf<TemplateInputs>{
                 OptionalValue("primary", &TemplateInputs::primary),
                 OptionalValue("context", &TemplateInputs::context,
                               InputFlow::AggregateByRequest),
                 OptionalValue("context_text", &TemplateInputs::context_text,
                               InputFlow::AggregateByRequest),
                 OptionalValue("matches", &TemplateInputs::matches,
                               InputFlow::AggregateByRequest),
                 OptionalValue("document", &TemplateInputs::document,
                               InputFlow::AggregateByRequest),
                 OptionalValue("document_text", &TemplateInputs::document_text,
                               InputFlow::AggregateByRequest),
                 OptionalValue("attributes", &TemplateInputs::attributes)},
             ProducedBatch<TextBatch>("text", PortFlow{}),
             std::move(parameters), RenderTemplate)
      .Category("common")
      .Description(
          "Text template rendering: {{name}} substitutes variables; "
          "single {name} and JSON braces remain literal")
      .PortConstraints(
          {PortGroupConstraint(PortConstraintKind::kAtLeastOneOf,
                               {"primary", "context", "context_text", "matches",
                                "document", "document_text", "attributes"},
                               "TextTemplateNode requires at least one dynamic "
                               "input port to be bound")})
      .ParallelSafe(true)
      .WithControl(std::move(control), UpdateTemplate);
}
}  // namespace

REGISTER_FUNCTION_NODE(TextTemplateNode, MakeTextTemplateSpec());

}  // namespace llm_edgeflow

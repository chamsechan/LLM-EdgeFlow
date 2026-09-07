#include <algorithm>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "company_alg_log.h"
#include "contracts/config_schema_validation.h"
#include "contracts/control_payload.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "engine/text/utf8.h"
#include "nodes/node_base.h"
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
      ConfigFieldDefinition{"template", ConfigValueKind::kString, false,
                            kDefaultTemplate},
      ConfigFieldDefinition{"separator", ConfigValueKind::kString, false,
                            kDefaultSeparator},
      ConfigFieldDefinition{"max_length", ConfigValueKind::kInteger, false,
                            kDefaultMaxLength, 1.0, 1048576.0},
      ConfigFieldDefinition{"allow_dynamic_attributes",
                            ConfigValueKind::kBoolean, false, false},
      ConfigFieldDefinition{"overflow_policy",
                            ConfigValueKind::kString,
                            false,
                            "fail",
                            std::nullopt,
                            std::nullopt,
                            {"fail", "truncate"}},
      ConfigFieldDefinition{"missing_variable_policy",
                            ConfigValueKind::kString,
                            false,
                            kDefaultMissingPolicy,
                            std::nullopt,
                            std::nullopt,
                            {"fail", "empty", "preserve"}},
      ConfigFieldDefinition{"values", ConfigValueKind::kObject, false}};
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
}  // namespace

/**
 * @brief 纯文本与多模态上下文受限模板渲染算子 (TextTemplateNode)
 */
class TextTemplateNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "TextTemplateNode";

  using TokenType = TextTemplateTokenType;
  using TemplateToken = TextTemplateToken;

  TextTemplateNode()
      : NodeBase(kNodeType),
        in_primary_("primary"),
        in_context_("context"),
        in_context_text_("context_text"),
        in_matches_("matches"),
        in_document_("document"),
        in_document_text_("document_text"),
        in_attributes_("attributes"),
        out_text_("text") {}

  static bool ValidateConfig(const nlohmann::json& config,
                             const std::unordered_set<std::string>& connected,
                             std::string* diagnostic) {
    std::unordered_map<std::string, std::string> values;
    if (config.contains("values"))
      values = config.at("values").get<decltype(values)>();
    std::vector<TemplateToken> compiled;
    return CompileTemplate(config.value("template", kDefaultTemplate), values,
                           connected.count("attributes") ||
                               config.value("allow_dynamic_attributes", false),
                           &compiled, diagnostic) &&
           ValidateTemplateInputs(
               compiled, connected,
               config.value("missing_variable_policy", kDefaultMissingPolicy),
               diagnostic);
  }

 protected:
  bool InitNode(const NodeInitContext& init_ctx, const nlohmann::json& config,
                SessionContext& /*session_ctx*/) override {
    BindPort(init_ctx, in_primary_);
    BindPort(init_ctx, in_context_);
    BindPort(init_ctx, in_context_text_);
    BindPort(init_ctx, in_matches_);
    BindPort(init_ctx, in_document_);
    BindPort(init_ctx, in_document_text_);
    BindPort(init_ctx, in_attributes_);
    BindPort(init_ctx, out_text_);

    nlohmann::json normalized_config;
    if (!ValidateAndNormalizeFields(TextTemplateConfigFields(), config,
                                    &normalized_config, nullptr)) {
      return false;
    }

    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    template_str_ = normalized_config.value("template", kDefaultTemplate);
    separator_ = normalized_config.value("separator", kDefaultSeparator);
    const int64_t configured_max_length =
        normalized_config.value<int64_t>("max_length", kDefaultMaxLength);
    if (configured_max_length < 1 || configured_max_length > 1048576) {
      return false;
    }
    max_length_ = static_cast<size_t>(configured_max_length);
    overflow_policy_ = normalized_config.value("overflow_policy", "fail");
    if (overflow_policy_ != "fail" && overflow_policy_ != "truncate") {
      return false;
    }

    static_values_.clear();
    if (normalized_config.contains("values")) {
      if (!normalized_config["values"].is_object()) return false;
      for (auto it = normalized_config["values"].begin();
           it != normalized_config["values"].end(); ++it) {
        if (!it.value().is_string()) return false;
        static_values_[it.key()] = it.value().get<std::string>();
      }
    }

    missing_variable_policy_ = normalized_config.value(
        "missing_variable_policy", kDefaultMissingPolicy);
    if (missing_variable_policy_ != "fail" &&
        missing_variable_policy_ != "empty" &&
        missing_variable_policy_ != "preserve") {
      return false;
    }
    allow_dynamic_attrs_ =
        in_attributes_.IsBound() ||
        normalized_config.value("allow_dynamic_attributes", false);

    std::vector<TemplateToken> compiled;
    if (!CompileTemplate(template_str_, static_values_, allow_dynamic_attrs_,
                         &compiled)) {
      return false;
    }
    connected_inputs_.reset();
    if (init_ctx.plan) {
      connected_inputs_.emplace();
      for (const auto& port : init_ctx.plan->ports) {
        if (port.direction == PortDirection::kInput)
          connected_inputs_->insert(port.logical_name);
      }
      if (!ValidateTemplateInputs(compiled, *connected_inputs_,
                                  missing_variable_policy_))
        return false;
    }
    compiled_tokens_ = std::move(compiled);
    return true;
  }

  NodeControlResult ControlNode(int cmd,
                                const std::string& json_param) override {
    if (cmd != kControlCmdUpdatePrompt) return NodeControlResult::Unsupported();
    nlohmann::json root;
    std::string error;
    if (!ParseControlPayload(json_param, TemplateControlSchema(), &root,
                             &error)) {
      return NodeControlResult::Failed(node_error::control::kInvalidRequest,
                                       error);
    }
    std::string new_tmpl;
    std::unordered_map<std::string, std::string> new_values;
    bool new_allow_dynamic;
    std::string new_missing_policy;
    std::string new_prompt_id;
    {
      std::shared_lock<std::shared_mutex> lock(rw_mutex_);
      new_tmpl = template_str_;
      new_values = static_values_;
      new_allow_dynamic = allow_dynamic_attrs_;
      new_missing_policy = missing_variable_policy_;
      new_prompt_id = prompt_id_;
    }
    if (root.contains("template"))
      new_tmpl = root["template"].get<std::string>();
    if (root.contains("allow_dynamic_attributes")) {
      new_allow_dynamic = in_attributes_.IsBound() ||
                          root["allow_dynamic_attributes"].get<bool>();
    }
    if (root.contains("missing_variable_policy")) {
      new_missing_policy = root["missing_variable_policy"].get<std::string>();
    }
    if (root.contains("prompt_id"))
      new_prompt_id = root["prompt_id"].get<std::string>();
    if (root.contains("values")) {
      for (auto it = root["values"].begin(); it != root["values"].end(); ++it) {
        new_values[it.key()] = it.value().get<std::string>();
      }
    }
    std::vector<TemplateToken> new_tokens;
    if (!CompileTemplate(new_tmpl, new_values, new_allow_dynamic,
                         &new_tokens) ||
        (connected_inputs_ &&
         !ValidateTemplateInputs(new_tokens, *connected_inputs_,
                                 new_missing_policy))) {
      return NodeControlResult::Failed(
          node_error::control::kInvalidRequest,
          "Invalid template placeholders or syntax in Control");
    }
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    template_str_ = std::move(new_tmpl);
    static_values_ = std::move(new_values);
    allow_dynamic_attrs_ = new_allow_dynamic;
    missing_variable_policy_ = std::move(new_missing_policy);
    prompt_id_ = std::move(new_prompt_id);
    compiled_tokens_ = std::move(new_tokens);
    return NodeControlResult::Handled();
  }

  int ProcessNode(AlgContext& req_ctx) override {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);

    const auto* primary_items = in_primary_.Get(req_ctx);
    const auto* context_items = in_context_.Get(req_ctx);
    const auto* context_text_items = in_context_text_.Get(req_ctx);
    const auto* matches_items = in_matches_.Get(req_ctx);
    const auto* document_items = in_document_.Get(req_ctx);
    const auto* document_text_items = in_document_text_.Get(req_ctx);
    const auto* attributes_items = in_attributes_.Get(req_ctx);

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
          if (i > 0) context_str += separator_;
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
          if (i > 0) doc_str += separator_;
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

      for (const auto& token : compiled_tokens_) {
        if (token.type == TokenType::kLiteral) {
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
          } else if (static_values_.find(var) != static_values_.end()) {
            value = &static_values_.at(var);
          }
          if (value) {
            rendered += *value;
          } else {
            if (missing_variable_policy_ == "fail") {
              return Fail(req_ctx, node_error::text_template::kMissingVariable,
                          "Missing required template variable: " + var);
            } else if (missing_variable_policy_ == "preserve") {
              rendered += "{" + var + "}";
            }
          }
        }
      }

      if (rendered.size() > max_length_) {
        if (overflow_policy_ == "fail") {
          return Fail(req_ctx,
                      node_error::text_template::kRenderedOutputTooLong,
                      "Rendered prompt exceeds max_length of " +
                          std::to_string(max_length_));
        }
        std::vector<size_t> boundaries;
        size_t invalid_offset = 0;
        if (!utf8::BuildCodePointBoundaries(rendered, &boundaries,
                                            &invalid_offset)) {
          return Fail(req_ctx, node_error::text_template::kInvalidUtf8,
                      "Rendered prompt contains invalid UTF-8 at byte offset " +
                          std::to_string(invalid_offset));
        }
        const auto boundary =
            std::upper_bound(boundaries.begin(), boundaries.end(), max_length_);
        rendered.resize(*(boundary - 1));
      }

      output_batch.emplace_back(req_id, sub_id, std::move(rendered));
    }

    out_text_.Set(req_ctx, std::move(output_batch));
    return 0;
  }

 private:
  static bool CompileTemplate(
      const std::string& tmpl,
      const std::unordered_map<std::string, std::string>& static_vals,
      bool allow_dynamic_attrs, std::vector<TemplateToken>* out_tokens,
      std::string* diagnostic = nullptr) {
    std::string error;
    bool ok = ParseTextTemplate(tmpl, out_tokens, &error);
    if (ok) {
      for (const auto& token : *out_tokens) {
        if (token.type == TokenType::kVariable && !allow_dynamic_attrs &&
            !BuiltinInputs().count(token.value) &&
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

  mutable std::shared_mutex rw_mutex_;
  std::string template_str_ = kDefaultTemplate;
  std::string separator_ = kDefaultSeparator;
  size_t max_length_ = kDefaultMaxLength;
  std::string overflow_policy_ = "fail";
  std::string missing_variable_policy_ = kDefaultMissingPolicy;
  std::optional<std::unordered_set<std::string>> connected_inputs_;
  std::string prompt_id_;
  bool allow_dynamic_attrs_ = false;
  std::unordered_map<std::string, std::string> static_values_;
  std::vector<TemplateToken> compiled_tokens_;

  BoundInput<TextBatch> in_primary_;
  BoundInput<RankedTextBatch> in_context_;
  BoundInput<TextBatch> in_context_text_;
  BoundInput<RuleMatchBatch> in_matches_;
  BoundInput<OcrDocumentBatch> in_document_;
  BoundInput<TextBatch> in_document_text_;
  BoundInput<TextAttributesBatch> in_attributes_;
  BoundOutput<TextBatch> out_text_;
};

NodeDefinition MakeTextTemplateNodeDefinition() {
  NodeDefinition def;
  def.node_type = TextTemplateNode::kNodeType;
  def.category = "common";
  def.validate_config = TextTemplateNode::ValidateConfig;
  def.description =
      "Text template rendering: {{name}} and {name} substitute variables; "
      "JSON braces remain literal";
  def.inputs = {
      OptionalInputPort("primary", BlackboardKey<TextBatch>{"", "TextBatch"},
                        "1:1", "preserve", "request"),
      OptionalInputPort("context",
                        BlackboardKey<RankedTextBatch>{"", "RankedTextBatch"},
                        "N:1", "aggregate", "request"),
      OptionalInputPort("context_text",
                        BlackboardKey<TextBatch>{"", "TextBatch"}, "N:1",
                        "aggregate", "request"),
      OptionalInputPort("matches",
                        BlackboardKey<RuleMatchBatch>{"", "RuleMatchBatch"},
                        "N:1", "aggregate", "request"),
      OptionalInputPort("document",
                        BlackboardKey<OcrDocumentBatch>{"", "OcrDocumentBatch"},
                        "N:1", "aggregate", "request"),
      OptionalInputPort("document_text",
                        BlackboardKey<TextBatch>{"", "TextBatch"}, "N:1",
                        "aggregate", "request"),
      OptionalInputPort(
          "attributes",
          BlackboardKey<TextAttributesBatch>{"", "TextAttributesBatch"}, "1:1",
          "preserve", "request")};
  def.outputs = {OutputPort("text", BlackboardKey<TextBatch>{"", "TextBatch"},
                            "1:1", "preserve", "request")};
  def.port_constraints = {PortGroupConstraint(
      PortConstraintKind::kAtLeastOneOf,
      {"primary", "context", "context_text", "matches", "document",
       "document_text", "attributes"},
      "TextTemplateNode requires at least one dynamic input port to be bound")};
  def.control_commands = {ControlCommandDefinition(
      kControlCmdUpdatePrompt, "update_prompt",
      "Update template string dynamically", TemplateControlSchema(), true)};
  def.control_commands.front().shared_id = true;
  def.config_fields = TextTemplateConfigFields();
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(TextTemplateNode,
                              MakeTextTemplateNodeDefinition());

}  // namespace llm_edgeflow

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

#include "contracts/control_payload.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "engine/text/utf8.h"
#include "nodes/configuration_snapshot.h"
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
  std::vector<TextTemplateToken> new_tokens;
  std::string diagnostic;
  if (!CompileTemplate(next.template_str, next.static_values,
                       next.allow_dynamic_attrs, &new_tokens, &diagnostic)) {
    return NodeResult<TemplateState>::Failure(
        NodeErrorKind::kBusinessError,
        diagnostic.empty()
            ? "Invalid template placeholders or syntax in Control"
            : diagnostic,
        node_error::control::kInvalidRequest);
  }
  if (bindings.has_bindings &&
      !ValidateTemplateInputs(new_tokens, bindings.connected_inputs,
                              next.missing_variable_policy, &diagnostic)) {
    return NodeResult<TemplateState>::Failure(
        NodeErrorKind::kBusinessError,
        diagnostic.empty()
            ? "Invalid template placeholders or syntax in Control"
            : diagnostic,
        node_error::control::kInvalidRequest);
  }
  next.compiled_tokens = std::move(new_tokens);
  return NodeResult<TemplateState>::Success(std::move(next));
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

    const auto& normalized_config = config;

    TemplateState initial_state;
    initial_state.template_str =
        normalized_config.value("template", kDefaultTemplate);
    initial_state.separator =
        normalized_config.value("separator", kDefaultSeparator);
    const int64_t configured_max_length =
        normalized_config.value<int64_t>("max_length", kDefaultMaxLength);
    if (configured_max_length < 1 || configured_max_length > 1048576) {
      return false;
    }
    initial_state.max_length = static_cast<size_t>(configured_max_length);
    initial_state.overflow_policy =
        normalized_config.value("overflow_policy", "fail");
    if (initial_state.overflow_policy != "fail" &&
        initial_state.overflow_policy != "truncate") {
      return false;
    }

    if (normalized_config.contains("values")) {
      if (!normalized_config["values"].is_object()) return false;
      for (auto it = normalized_config["values"].begin();
           it != normalized_config["values"].end(); ++it) {
        if (!it.value().is_string()) return false;
        initial_state.static_values[it.key()] = it.value().get<std::string>();
      }
    }

    initial_state.missing_variable_policy = normalized_config.value(
        "missing_variable_policy", kDefaultMissingPolicy);
    if (initial_state.missing_variable_policy != "fail" &&
        initial_state.missing_variable_policy != "empty" &&
        initial_state.missing_variable_policy != "preserve") {
      return false;
    }
    binding_facts_ = MakeBindingFacts(init_ctx);
    if (in_attributes_.IsBound()) {
      binding_facts_.connected_inputs.insert("attributes");
    }
    initial_state.allow_dynamic_attrs =
        binding_facts_.IsConnected("attributes") ||
        normalized_config.value("allow_dynamic_attributes", false);

    std::vector<TemplateToken> compiled;
    if (!CompileTemplate(initial_state.template_str,
                         initial_state.static_values,
                         initial_state.allow_dynamic_attrs, &compiled)) {
      return false;
    }
    if (!ValidateTemplateInputs(compiled, binding_facts_.connected_inputs,
                                initial_state.missing_variable_policy))
      return false;
    initial_state.compiled_tokens = std::move(compiled);
    snapshot_.Initialize(std::move(initial_state));
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
    return snapshot_.Update([&](const TemplateState& current) {
      return BuildNextTemplate(current, update, binding_facts_);
    });
  }

  int ProcessNode(AlgContext& req_ctx) override {
    auto state_guard = snapshot_.Read();
    if (!state_guard) {
      return Fail(req_ctx, node_error::author_node::kInternalError,
                  "Snapshot uninitialized");
    }
    const auto& state = *state_guard;

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
          } else if (state.static_values.find(var) !=
                     state.static_values.end()) {
            value = &state.static_values.at(var);
          }
          if (value) {
            rendered += *value;
          } else {
            if (state.missing_variable_policy == "fail") {
              return Fail(req_ctx, node_error::text_template::kMissingVariable,
                          "Missing required template variable: " + var);
            } else if (state.missing_variable_policy == "preserve") {
              rendered += "{{" + var + "}}";
            }
          }
        }
      }

      if (rendered.size() > state.max_length) {
        if (state.overflow_policy == "fail") {
          return Fail(req_ctx,
                      node_error::text_template::kRenderedOutputTooLong,
                      "Rendered prompt exceeds max_length of " +
                          std::to_string(state.max_length));
        }
        std::vector<size_t> boundaries;
        size_t invalid_offset = 0;
        if (!utf8::BuildCodePointBoundaries(rendered, &boundaries,
                                            &invalid_offset)) {
          return Fail(req_ctx, node_error::text_template::kInvalidUtf8,
                      "Rendered prompt contains invalid UTF-8 at byte offset " +
                          std::to_string(invalid_offset));
        }
        const auto boundary = std::upper_bound(
            boundaries.begin(), boundaries.end(), state.max_length);
        rendered.resize(*(boundary - 1));
      }

      output_batch.emplace_back(req_id, sub_id, std::move(rendered));
    }

    out_text_.Set(req_ctx, std::move(output_batch));
    return 0;
  }

 private:
  ConfigurationSnapshot<TemplateState> snapshot_;
  BindingFacts binding_facts_;

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
      "Text template rendering: {{name}} substitutes variables; "
      "single {name} and JSON braces remain literal";
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

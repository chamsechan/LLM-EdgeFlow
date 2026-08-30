#include <algorithm>
#include <mutex>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "company_alg_log.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "engine/text/utf8.h"
#include "nodes/node_error_codes.h"
#include "nodes/node_support.h"

namespace alg_framework {

/**
 * @brief 纯文本与多模态上下文受限模板渲染算子 (TextTemplateNode)
 */
class TextTemplateNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "TextTemplateNode";

  enum class TokenType { kLiteral, kVariable };
  struct TemplateToken {
    TokenType type = TokenType::kLiteral;
    std::string value;
  };

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

    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    template_str_ = config.value("template", "{{primary}}");
    separator_ = config.value("separator", "\n");
    const int64_t configured_max_length = config.value("max_length", 65536);
    if (configured_max_length < 1 || configured_max_length > 1048576) {
      return false;
    }
    max_length_ = static_cast<size_t>(configured_max_length);
    overflow_policy_ = config.value("overflow_policy", "fail");
    if (overflow_policy_ != "fail" && overflow_policy_ != "truncate") {
      return false;
    }

    static_values_.clear();
    if (config.contains("values")) {
      if (!config["values"].is_object()) return false;
      for (auto it = config["values"].begin(); it != config["values"].end();
           ++it) {
        if (!it.value().is_string()) return false;
        static_values_[it.key()] = it.value().get<std::string>();
      }
    }

    missing_variable_policy_ = config.value("missing_variable_policy", "fail");
    if (missing_variable_policy_ != "fail" &&
        missing_variable_policy_ != "empty" &&
        missing_variable_policy_ != "preserve") {
      return false;
    }
    allow_dynamic_attrs_ = in_attributes_.IsBound() ||
                           config.value("allow_dynamic_attributes", false);

    std::vector<TemplateToken> compiled;
    if (!CompileTemplate(template_str_, static_values_, allow_dynamic_attrs_,
                         &compiled)) {
      return false;
    }
    compiled_tokens_ = std::move(compiled);
    return true;
  }

  NodeControlResult ControlNode(int cmd,
                                const std::string& json_param) override {
    if (cmd == kControlCmdUpdatePrompt) {
      try {
        nlohmann::json root = nlohmann::json::parse(json_param);
        if (!root.is_object()) {
          return NodeControlResult::Failed(node_error::control::kInvalidRequest,
                                           "Control payload must be an object");
        }
        static const std::unordered_set<std::string> kAllowedFields = {
            "template", "prompt_id", "values", "allow_dynamic_attributes",
            "missing_variable_policy"};
        for (auto it = root.begin(); it != root.end(); ++it) {
          if (!kAllowedFields.count(it.key())) {
            return NodeControlResult::Failed(
                node_error::control::kInvalidRequest,
                "Unknown field in Control payload: " + it.key());
          }
        }
        if (!root.contains("template") && !root.contains("values") &&
            !root.contains("allow_dynamic_attributes") &&
            !root.contains("missing_variable_policy") &&
            !root.contains("prompt_id")) {
          return NodeControlResult::Failed(
              node_error::control::kInvalidRequest,
              "No recognized update field in Control payload");
        }
        if ((root.contains("template") && !root["template"].is_string()) ||
            (root.contains("prompt_id") && !root["prompt_id"].is_string()) ||
            (root.contains("values") && !root["values"].is_object()) ||
            (root.contains("allow_dynamic_attributes") &&
             !root["allow_dynamic_attributes"].is_boolean()) ||
            (root.contains("missing_variable_policy") &&
             !root["missing_variable_policy"].is_string())) {
          return NodeControlResult::Failed(
              node_error::control::kInvalidRequest,
              "Control payload field has an invalid type");
        }

        std::string new_tmpl;
        std::unordered_map<std::string, std::string> new_values;
        bool new_allow_dynamic = allow_dynamic_attrs_;
        std::string new_missing_policy = missing_variable_policy_;
        std::string new_prompt_id = prompt_id_;

        {
          std::shared_lock<std::shared_mutex> lock(rw_mutex_);
          new_tmpl = template_str_;
          new_values = static_values_;
          new_allow_dynamic = allow_dynamic_attrs_;
          new_missing_policy = missing_variable_policy_;
          new_prompt_id = prompt_id_;
        }

        if (root.contains("template") && root["template"].is_string()) {
          new_tmpl = root["template"].get<std::string>();
        }
        if (root.contains("allow_dynamic_attributes") &&
            root["allow_dynamic_attributes"].is_boolean()) {
          new_allow_dynamic = root["allow_dynamic_attributes"].get<bool>();
        }
        if (root.contains("missing_variable_policy") &&
            root["missing_variable_policy"].is_string()) {
          new_missing_policy =
              root["missing_variable_policy"].get<std::string>();
          if (new_missing_policy != "fail" && new_missing_policy != "empty" &&
              new_missing_policy != "preserve") {
            return NodeControlResult::Failed(
                node_error::control::kInvalidRequest,
                "Invalid missing_variable_policy in Control payload");
          }
        }
        if (root.contains("prompt_id") && root["prompt_id"].is_string()) {
          new_prompt_id = root["prompt_id"].get<std::string>();
        }
        if (root.contains("values") && root["values"].is_object()) {
          for (auto it = root["values"].begin(); it != root["values"].end();
               ++it) {
            if (!it.value().is_string()) {
              return NodeControlResult::Failed(
                  node_error::control::kInvalidRequest,
                  "Control values entries must be strings");
            }
            new_values[it.key()] = it.value().get<std::string>();
          }
        }

        std::vector<TemplateToken> new_tokens;
        if (!CompileTemplate(new_tmpl, new_values, new_allow_dynamic,
                             &new_tokens)) {
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
        return NodeControlResult::Handled(0, "Template updated successfully");
      } catch (const std::exception& e) {
        return NodeControlResult::Failed(
            node_error::control::kInvalidRequest,
            std::string("JSON parse error: ") + e.what());
      }
    }
    return NodeControlResult::Unsupported();
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
          if (var == "primary" || var == "text") {
            rendered += primary_str;
          } else if (var == "context" || var == "context_text") {
            rendered += context_str;
          } else if (var == "matches") {
            rendered += matches_str;
          } else if (var == "document" || var == "document_text") {
            rendered += doc_str;
          } else if (attrs_ptr && attrs_ptr->find(var) != attrs_ptr->end()) {
            rendered += attrs_ptr->at(var);
          } else if (static_values_.find(var) != static_values_.end()) {
            rendered += static_values_.at(var);
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
  static bool IsValidIdentifier(const std::string& str) {
    if (str.empty()) return false;
    for (char c : str) {
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_')) {
        return false;
      }
    }
    return true;
  }

  static bool CompileTemplate(
      const std::string& tmpl,
      const std::unordered_map<std::string, std::string>& static_vals,
      bool allow_dynamic_attrs, std::vector<TemplateToken>* out_tokens) {
    static const std::unordered_set<std::string> kBuiltins = {
        "primary", "context",  "context_text",
        "matches", "document", "document_text"};

    out_tokens->clear();
    size_t pos = 0;
    while (pos < tmpl.size()) {
      size_t open_pos = tmpl.find('{', pos);
      if (open_pos == std::string::npos) {
        out_tokens->push_back({TokenType::kLiteral, tmpl.substr(pos)});
        break;
      }

      if (open_pos > pos) {
        out_tokens->push_back(
            {TokenType::kLiteral, tmpl.substr(pos, open_pos - pos)});
      }

      bool is_double =
          (open_pos + 1 < tmpl.size() && tmpl[open_pos + 1] == '{');

      if (is_double) {
        size_t close_pos = tmpl.find("}}", open_pos + 2);
        if (close_pos == std::string::npos) {
          ALG_LOG_ERROR(
              "[TextTemplateNode] Unclosed {{ placeholder in template at "
              "%zu\n",
              open_pos);
          return false;
        }
        std::string raw_name =
            tmpl.substr(open_pos + 2, close_pos - open_pos - 2);
        size_t first = raw_name.find_first_not_of(" \t");
        size_t last = raw_name.find_last_not_of(" \t");
        if (first == std::string::npos) {
          ALG_LOG_ERROR(
              "[TextTemplateNode] Empty {{}} placeholder in template\n");
          return false;
        }
        std::string var_name = raw_name.substr(first, last - first + 1);
        if (!IsValidIdentifier(var_name) ||
            (!allow_dynamic_attrs && !kBuiltins.count(var_name) &&
             !static_vals.count(var_name))) {
          ALG_LOG_ERROR("[TextTemplateNode] Unknown template placeholder: %s\n",
                        var_name.c_str());
          return false;
        }
        out_tokens->push_back({TokenType::kVariable, std::move(var_name)});
        pos = close_pos + 2;
        continue;
      }

      // Single { : check if it forms a valid identifier placeholder {var}
      size_t close_pos = tmpl.find('}', open_pos + 1);
      if (close_pos != std::string::npos) {
        std::string raw_name =
            tmpl.substr(open_pos + 1, close_pos - open_pos - 1);
        size_t first = raw_name.find_first_not_of(" \t");
        size_t last = raw_name.find_last_not_of(" \t");
        if (first != std::string::npos) {
          std::string var_name = raw_name.substr(first, last - first + 1);
          if (IsValidIdentifier(var_name)) {
            if (!allow_dynamic_attrs && !kBuiltins.count(var_name) &&
                !static_vals.count(var_name)) {
              ALG_LOG_ERROR(
                  "[TextTemplateNode] Unknown template placeholder: %s\n",
                  var_name.c_str());
              return false;
            }
            out_tokens->push_back({TokenType::kVariable, std::move(var_name)});
            pos = close_pos + 1;
            continue;
          }
        }
      }

      // Not a valid variable placeholder (e.g. JSON literal '{'), emit literal
      // '{'
      out_tokens->push_back({TokenType::kLiteral, "{"});
      pos = open_pos + 1;
    }
    return true;
  }

  mutable std::shared_mutex rw_mutex_;
  std::string template_str_ = "{{primary}}";
  std::string separator_ = "\n";
  size_t max_length_ = 65536;
  std::string overflow_policy_ = "fail";
  std::string missing_variable_policy_ = "fail";
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
  def.description = "Text template rendering and prompt builder node";
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
      "Update template string dynamically",
      nlohmann::json{
          {"type", "object"},
          {"minProperties", 1},
          {"additionalProperties", false},
          {"properties",
           {{"template", {{"type", "string"}}},
            {"prompt_id", {{"type", "string"}}},
            {"values",
             {{"type", "object"},
              {"additionalProperties", {{"type", "string"}}}}},
            {"allow_dynamic_attributes", {{"type", "boolean"}}},
            {"missing_variable_policy",
             {{"type", "string"}, {"enum", {"fail", "empty", "preserve"}}}}}}},
      true)};
  def.config_fields = {
      ConfigFieldDefinition{"template", ConfigValueKind::kString, false,
                            "{{primary}}"},
      ConfigFieldDefinition{"separator", ConfigValueKind::kString, false, "\n"},
      ConfigFieldDefinition{"max_length", ConfigValueKind::kInteger, false,
                            65536, 1.0, 1048576.0},
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
                            "fail",
                            std::nullopt,
                            std::nullopt,
                            {"fail", "empty", "preserve"}},
      ConfigFieldDefinition{"values", ConfigValueKind::kObject, false}};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(TextTemplateNode,
                              MakeTextTemplateNodeDefinition());

}  // namespace alg_framework

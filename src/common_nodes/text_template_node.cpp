#include <algorithm>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <regex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/common_contracts.h"
#include "core/node_registry.h"
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
    max_length_ = config.value("max_length", 65536);
    overflow_policy_ = config.value("overflow_policy", "fail");

    static_values_.clear();
    if (config.contains("values") && config["values"].is_object()) {
      for (auto it = config["values"].begin(); it != config["values"].end();
           ++it) {
        if (it.value().is_string()) {
          static_values_[it.key()] = it.value().get<std::string>();
        }
      }
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
        std::string new_tmpl;
        std::unordered_map<std::string, std::string> new_values;

        {
          std::shared_lock<std::shared_mutex> lock(rw_mutex_);
          new_tmpl = template_str_;
          new_values = static_values_;
        }

        if (root.contains("template") && root["template"].is_string()) {
          new_tmpl = root["template"].get<std::string>();
        }
        if (root.contains("values") && root["values"].is_object()) {
          for (auto it = root["values"].begin(); it != root["values"].end();
               ++it) {
            if (it.value().is_string()) {
              new_values[it.key()] = it.value().get<std::string>();
            }
          }
        }

        std::vector<TemplateToken> new_tokens;
        if (!CompileTemplate(new_tmpl, new_values, allow_dynamic_attrs_,
                             &new_tokens)) {
          return NodeControlResult::Failed(
              -1, "Invalid template placeholders or syntax in Control");
        }

        std::unique_lock<std::shared_mutex> lock(rw_mutex_);
        template_str_ = std::move(new_tmpl);
        static_values_ = std::move(new_values);
        compiled_tokens_ = std::move(new_tokens);
        return NodeControlResult::Handled(0, "Template updated successfully");
      } catch (const std::exception& e) {
        return NodeControlResult::Failed(
            -1, std::string("JSON parse error: ") + e.what());
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

    if (primary_items) {
      for (const auto& item : *primary_items) {
        record_sample(item.req_id, item.sub_id);
        primary_by_sample[{item.req_id, item.sub_id}] = item.data;
      }
    }

    if (context_items) {
      for (const auto& item : *context_items) {
        record_sample(item.req_id, item.sub_id);
        context_by_req[item.req_id].push_back(item.data.text);
      }
    }

    if (context_text_items) {
      for (const auto& item : *context_text_items) {
        record_sample(item.req_id, item.sub_id);
        context_by_req[item.req_id].push_back(item.data);
      }
    }

    if (matches_items) {
      for (const auto& item : *matches_items) {
        record_sample(item.req_id, item.sub_id);
        if (!item.data.matched_word.empty()) {
          matches_by_req[item.req_id].push_back(item.data.matched_word);
        } else if (!item.data.category.empty()) {
          matches_by_req[item.req_id].push_back(item.data.category);
        }
      }
    }

    if (document_items) {
      for (const auto& item : *document_items) {
        record_sample(item.req_id, item.sub_id);
        document_by_req[item.req_id].push_back(item.data.combined_text);
      }
    }

    if (document_text_items) {
      for (const auto& item : *document_text_items) {
        record_sample(item.req_id, item.sub_id);
        document_by_req[item.req_id].push_back(item.data);
      }
    }

    if (attributes_items) {
      for (const auto& item : *attributes_items) {
        record_sample(item.req_id, item.sub_id);
        attributes_by_sample[{item.req_id, item.sub_id}] = item.data;
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
          }
        }
      }

      if (rendered.size() > max_length_) {
        if (overflow_policy_ == "fail") {
          return Fail(req_ctx, -6201,
                      "Rendered prompt exceeds max_length of " +
                          std::to_string(max_length_));
        }
        rendered.resize(max_length_);
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
        "primary", "text",     "context",       "context_text",
        "matches", "document", "document_text", "attributes"};

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
          std::cerr
              << "[TextTemplateNode] Unclosed {{ placeholder in template at "
              << open_pos << std::endl;
          return false;
        }
        std::string raw_name =
            tmpl.substr(open_pos + 2, close_pos - open_pos - 2);
        size_t first = raw_name.find_first_not_of(" \t");
        size_t last = raw_name.find_last_not_of(" \t");
        if (first == std::string::npos) {
          std::cerr << "[TextTemplateNode] Empty {{}} placeholder in template"
                    << std::endl;
          return false;
        }
        std::string var_name = raw_name.substr(first, last - first + 1);
        if (!IsValidIdentifier(var_name) ||
            (!allow_dynamic_attrs && !kBuiltins.count(var_name) &&
             !static_vals.count(var_name))) {
          std::cerr << "[TextTemplateNode] Unknown template placeholder: "
                    << var_name << std::endl;
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
              std::cerr << "[TextTemplateNode] Unknown template placeholder: "
                        << var_name << std::endl;
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
                            false, "1:1", "preserve", "request")};
  def.port_constraints = {PortGroupConstraint(
      PortConstraintKind::kAtLeastOneOf,
      {"primary", "context", "context_text", "matches", "document",
       "document_text", "attributes"},
      "TextTemplateNode requires at least one dynamic input port to be bound")};
  def.control_commands = {ControlCommandDefinition(
      kControlCmdUpdatePrompt, "update_prompt",
      "Update template string dynamically",
      nlohmann::json{{"type", "object"},
                     {"properties",
                      {{"template", {{"type", "string"}}},
                       {"values", {{"type", "object"}}},
                       {"allow_dynamic_attributes", {{"type", "boolean"}}}}}},
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
      ConfigFieldDefinition{"values", ConfigValueKind::kObject, false}};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(TextTemplateNode,
                              MakeTextTemplateNodeDefinition());

}  // namespace alg_framework

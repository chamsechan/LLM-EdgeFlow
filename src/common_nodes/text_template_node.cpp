#include <algorithm>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <regex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

  TextTemplateNode()
      : NodeBase(kNodeType),
        in_primary_("primary"),
        in_context_("context"),
        in_context_text_("context_text"),
        in_matches_("matches"),
        in_document_("document"),
        in_document_text_("document_text"),
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
    BindPort(init_ctx, out_text_);

    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    template_str_ = config.value("template", "{{primary}}");
    separator_ = config.value("separator", "\n");
    max_length_ = config.value("max_length", 65536);
    overflow_policy_ = config.value("overflow_policy", "fail");

    if (config.contains("values") && config["values"].is_object()) {
      static_values_.clear();
      for (auto it = config["values"].begin(); it != config["values"].end();
           ++it) {
        if (it.value().is_string()) {
          static_values_[it.key()] = it.value().get<std::string>();
        }
      }
    }

    return ValidateTemplate(template_str_, static_values_);
  }

  NodeControlResult ControlNode(int cmd,
                                const std::string& json_param) override {
    if (cmd == kControlCmdUpdatePrompt) {
      try {
        nlohmann::json root = nlohmann::json::parse(json_param);
        std::unique_lock<std::shared_mutex> lock(rw_mutex_);
        std::string new_tmpl = template_str_;
        std::unordered_map<std::string, std::string> new_values =
            static_values_;

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

        if (!ValidateTemplate(new_tmpl, new_values)) {
          return NodeControlResult::Failed(-1, "Invalid template placeholders");
        }

        template_str_ = std::move(new_tmpl);
        static_values_ = std::move(new_values);
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

    // 计算请求批次大小与各请求对应的元素集合
    std::unordered_map<uint32_t, std::string> primary_by_req;
    std::unordered_map<uint32_t, std::vector<std::string>> context_by_req;
    std::unordered_map<uint32_t, std::vector<std::string>> matches_by_req;
    std::unordered_map<uint32_t, std::vector<std::string>> document_by_req;
    std::vector<uint32_t> ordered_req_ids;
    std::unordered_map<uint32_t, uint32_t> sub_ids_by_req;

    auto record_req = [&](uint32_t req_id, uint32_t sub_id) {
      if (std::find(ordered_req_ids.begin(), ordered_req_ids.end(), req_id) ==
          ordered_req_ids.end()) {
        ordered_req_ids.push_back(req_id);
        sub_ids_by_req[req_id] = sub_id;
      }
    };

    if (primary_items) {
      for (const auto& item : *primary_items) {
        record_req(item.req_id, item.sub_id);
        primary_by_req[item.req_id] = item.data;
      }
    }

    if (context_items) {
      for (const auto& item : *context_items) {
        record_req(item.req_id, item.sub_id);
        context_by_req[item.req_id].push_back(item.data.text);
      }
    }

    if (context_text_items) {
      for (const auto& item : *context_text_items) {
        record_req(item.req_id, item.sub_id);
        context_by_req[item.req_id].push_back(item.data);
      }
    }

    if (matches_items) {
      for (const auto& item : *matches_items) {
        record_req(item.req_id, item.sub_id);
        if (!item.data.matched_word.empty()) {
          matches_by_req[item.req_id].push_back(item.data.matched_word);
        } else if (!item.data.category.empty()) {
          matches_by_req[item.req_id].push_back(item.data.category);
        }
      }
    }

    if (document_items) {
      for (const auto& item : *document_items) {
        record_req(item.req_id, item.sub_id);
        document_by_req[item.req_id].push_back(item.data.combined_text);
      }
    }

    if (document_text_items) {
      for (const auto& item : *document_text_items) {
        record_req(item.req_id, item.sub_id);
        document_by_req[item.req_id].push_back(item.data);
      }
    }

    TextBatch output_batch;
    output_batch.reserve(ordered_req_ids.size());

    for (uint32_t req_id : ordered_req_ids) {
      std::string rendered = template_str_;

      // 1. 替换 primary / text
      std::string primary_str;
      if (primary_by_req.find(req_id) != primary_by_req.end()) {
        primary_str = primary_by_req[req_id];
      }
      ReplaceAll(rendered, "{{primary}}", primary_str);
      ReplaceAll(rendered, "{primary}", primary_str);
      ReplaceAll(rendered, "{text}", primary_str);
      ReplaceAll(rendered, "{{text}}", primary_str);

      // 2. 替换 context
      std::string context_str;
      if (context_by_req.find(req_id) != context_by_req.end()) {
        const auto& list = context_by_req[req_id];
        for (size_t i = 0; i < list.size(); ++i) {
          if (i > 0) context_str += separator_;
          context_str += list[i];
        }
      }
      ReplaceAll(rendered, "{{context}}", context_str);
      ReplaceAll(rendered, "{context}", context_str);

      // 3. 替换 matches
      std::string matches_str;
      if (matches_by_req.find(req_id) != matches_by_req.end()) {
        const auto& list = matches_by_req[req_id];
        for (size_t i = 0; i < list.size(); ++i) {
          if (i > 0) matches_str += ", ";
          matches_str += list[i];
        }
      }
      ReplaceAll(rendered, "{{matches}}", matches_str);
      ReplaceAll(rendered, "{matches}", matches_str);

      // 4. 替换 document
      std::string doc_str;
      if (document_by_req.find(req_id) != document_by_req.end()) {
        const auto& list = document_by_req[req_id];
        for (size_t i = 0; i < list.size(); ++i) {
          if (i > 0) doc_str += separator_;
          doc_str += list[i];
        }
      }
      ReplaceAll(rendered, "{{document}}", doc_str);
      ReplaceAll(rendered, "{document}", doc_str);

      // 5. 替换 static_values
      for (const auto& [k, v] : static_values_) {
        ReplaceAll(rendered, "{{" + k + "}}", v);
        ReplaceAll(rendered, "{" + k + "}", v);
      }

      if (rendered.size() > max_length_) {
        if (overflow_policy_ == "fail") {
          return Fail(req_ctx, -6201,
                      "Rendered prompt exceeds max_length of " +
                          std::to_string(max_length_));
        }
        rendered.resize(max_length_);
      }

      output_batch.emplace_back(req_id, sub_ids_by_req[req_id],
                                std::move(rendered));
    }

    out_text_.Set(req_ctx, std::move(output_batch));
    return 0;
  }

 private:
  static bool ValidateTemplate(
      const std::string& tmpl,
      const std::unordered_map<std::string, std::string>& static_vals) {
    static const std::unordered_set<std::string> kBuiltins = {
        "primary", "text", "context", "matches", "document", "document_text"};
    // 匹配 {{var}} 或 {var}
    std::regex re(R"(\{\{([a-zA-Z0-9_]+)\}\})");
    auto words_begin = std::sregex_iterator(tmpl.begin(), tmpl.end(), re);
    auto words_end = std::sregex_iterator();
    for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
      std::smatch match = *i;
      std::string var_name = match[1].str();
      if (!kBuiltins.count(var_name) && !static_vals.count(var_name)) {
        std::cerr << "[TextTemplateNode] Unknown template placeholder: "
                  << var_name << std::endl;
        return false;
      }
    }
    return true;
  }

  static void ReplaceAll(std::string& str, const std::string& from,
                         const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
      str.replace(pos, from.length(), to);
      pos += to.length();
    }
  }

  mutable std::shared_mutex rw_mutex_;
  std::string template_str_ = "{{primary}}";
  std::string separator_ = "\n";
  size_t max_length_ = 65536;
  std::string overflow_policy_ = "fail";
  std::unordered_map<std::string, std::string> static_values_;

  BoundInput<TextBatch> in_primary_;
  BoundInput<RankedTextBatch> in_context_;
  BoundInput<TextBatch> in_context_text_;
  BoundInput<RuleMatchBatch> in_matches_;
  BoundInput<OcrDocumentBatch> in_document_;
  BoundInput<TextBatch> in_document_text_;
  BoundOutput<TextBatch> out_text_;
};

NodeDefinition MakeTextTemplateNodeDefinition() {
  NodeDefinition def;
  def.node_type = TextTemplateNode::kNodeType;
  def.category = "common";
  def.description = "Text template rendering and prompt builder node";
  def.inputs = {
      OptionalInputPort("primary", BlackboardKey<TextBatch>{"", "TextBatch"}),
      OptionalInputPort("context",
                        BlackboardKey<RankedTextBatch>{"", "RankedTextBatch"}),
      OptionalInputPort("context_text",
                        BlackboardKey<TextBatch>{"", "TextBatch"}),
      OptionalInputPort("matches",
                        BlackboardKey<RuleMatchBatch>{"", "RuleMatchBatch"}),
      OptionalInputPort(
          "document", BlackboardKey<OcrDocumentBatch>{"", "OcrDocumentBatch"}),
      OptionalInputPort("document_text",
                        BlackboardKey<TextBatch>{"", "TextBatch"})};
  def.outputs = {OutputPort("text", BlackboardKey<TextBatch>{"", "TextBatch"})};
  def.port_constraints = {PortGroupConstraint(
      PortConstraintKind::kAtLeastOneOf,
      {"primary", "context", "context_text", "matches", "document",
       "document_text"},
      "TextTemplateNode requires at least one dynamic input port to be bound")};
  def.control_commands = {ControlCommandDefinition(
      kControlCmdUpdatePrompt, "update_prompt",
      "Update template string dynamically", nlohmann::json::object(), true)};
  def.config_fields = {
      ConfigFieldDefinition{"template", ConfigValueKind::kString, false,
                            "{{primary}}"},
      ConfigFieldDefinition{"separator", ConfigValueKind::kString, false, "\n"},
      ConfigFieldDefinition{"max_length", ConfigValueKind::kInteger, false,
                            65536, 1.0, 1048576.0},
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

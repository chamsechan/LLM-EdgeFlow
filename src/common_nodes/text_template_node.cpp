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
 * @brief 纯文本与多模态上下文受限模板渲染算子 (TextTemplateNode)
 */
class TextTemplateNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "TextTemplateNode";

  TextTemplateNode()
      : NodeBase(kNodeType),
        in_primary_("primary", "primary", "TextBatch"),
        in_context_("context", "context", "RankedTextBatch"),
        in_context_text_("context_text", "context_text", "TextBatch"),
        in_matches_("matches", "matches", "RuleMatchBatch"),
        in_document_("document", "document", "OcrDocumentBatch"),
        in_document_text_("document_text", "document_text", "TextBatch"),
        out_text_("text", "text", "TextBatch") {}

 protected:
  bool InitNode(const nlohmann::json& config,
                SessionContext& /*session_ctx*/) override {
    BindPort(in_primary_);
    BindPort(in_context_);
    BindPort(in_context_text_);
    BindPort(in_matches_);
    BindPort(in_document_);
    BindPort(in_document_text_);
    BindPort(out_text_);

    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    template_str_ = config.value("template", "{{primary}}");
    separator_ = config.value("separator", "\n");
    max_length_ = config.value("max_length", 65536);

    if (config.contains("values") && config["values"].is_object()) {
      static_values_.clear();
      for (auto it = config["values"].begin(); it != config["values"].end();
           ++it) {
        if (it.value().is_string()) {
          static_values_[it.key()] = it.value().get<std::string>();
        }
      }
    }
    return true;
  }

  NodeControlResult ControlNode(int cmd,
                                const std::string& json_param) override {
    if (cmd == 1 || cmd == 2) {
      try {
        nlohmann::json root = nlohmann::json::parse(json_param);
        std::unique_lock<std::shared_mutex> lock(rw_mutex_);
        if (root.contains("template") && root["template"].is_string()) {
          template_str_ = root["template"].get<std::string>();
        }
        if (root.contains("values") && root["values"].is_object()) {
          for (auto it = root["values"].begin(); it != root["values"].end();
               ++it) {
            if (it.value().is_string()) {
              static_values_[it.key()] = it.value().get<std::string>();
            }
          }
        }
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
        rendered.resize(max_length_);
      }

      output_batch.emplace_back(req_id, sub_ids_by_req[req_id],
                                std::move(rendered));
    }

    out_text_.Set(req_ctx, std::move(output_batch));
    return 0;
  }

 private:
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
  def.config_fields = {
      ConfigFieldDefinition{"template", ConfigValueKind::kString, false,
                            "{{primary}}"},
      ConfigFieldDefinition{"separator", ConfigValueKind::kString, false, "\n"},
      ConfigFieldDefinition{"max_length", ConfigValueKind::kInteger, false,
                            65536, 1.0, 1048576.0},
      ConfigFieldDefinition{"values", ConfigValueKind::kObject, false}};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(TextTemplateNode,
                              MakeTextTemplateNodeDefinition());

}  // namespace alg_framework

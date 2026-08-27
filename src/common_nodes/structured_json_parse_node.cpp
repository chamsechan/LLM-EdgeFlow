#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "nodes/node_support.h"

namespace alg_framework {

/**
 * @brief 结构化 JSON 解析与文本提取受控算子 (StructuredJsonParseNode)
 */
class StructuredJsonParseNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "StructuredJsonParseNode";

  StructuredJsonParseNode()
      : NodeBase(kNodeType),
        in_text_("text", "text", "TextBatch"),
        out_doc_("document", "document", "StructuredDocumentBatch") {}

 protected:
  bool InitNode(const nlohmann::json& config,
                SessionContext& /*session_ctx*/) override {
    BindPort(in_text_);
    BindPort(out_doc_);

    fallback_json_ = config.value("fallback_json", "{}");
    extract_json_block_ = config.value("extract_json_block", true);
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* text_items =
        in_text_.Require(req_ctx, -6101, "StructuredJsonParseNode input");
    if (!text_items) {
      return -6101;
    }

    StructuredDocumentBatch output_docs;
    output_docs.reserve(text_items->size());

    for (const auto& item : *text_items) {
      std::string parsed_json_str = ParseOrExtractJson(item.data);
      output_docs.emplace_back(item.req_id, item.sub_id,
                               std::move(parsed_json_str));
    }

    out_doc_.Set(req_ctx, std::move(output_docs));
    return 0;
  }

 private:
  std::string ParseOrExtractJson(const std::string& input) const {
    if (input.empty()) return fallback_json_;

    // 1. 尝试直接完整解析
    try {
      auto j = nlohmann::json::parse(input);
      return j.dump();
    } catch (...) {
    }

    if (!extract_json_block_) {
      return fallback_json_;
    }

    // 2. 尝试从 markdown 代码块 ```json ... ``` 提取
    size_t code_block_start = input.find("```json");
    if (code_block_start != std::string::npos) {
      size_t content_start = code_block_start + 7;
      size_t code_block_end = input.find("```", content_start);
      if (code_block_end != std::string::npos) {
        std::string candidate =
            input.substr(content_start, code_block_end - content_start);
        try {
          auto j = nlohmann::json::parse(candidate);
          return j.dump();
        } catch (...) {
        }
      }
    }

    // 3. 尝试搜索最外层 { ... } 或 [ ... ]
    size_t obj_start = input.find('{');
    size_t obj_end = input.rfind('}');
    if (obj_start != std::string::npos && obj_end != std::string::npos &&
        obj_end > obj_start) {
      std::string candidate = input.substr(obj_start, obj_end - obj_start + 1);
      try {
        auto j = nlohmann::json::parse(candidate);
        return j.dump();
      } catch (...) {
      }
    }

    size_t arr_start = input.find('[');
    size_t arr_end = input.rfind(']');
    if (arr_start != std::string::npos) {
      if (arr_end != std::string::npos && arr_end > arr_start) {
        std::string candidate =
            input.substr(arr_start, arr_end - arr_start + 1);
        try {
          auto j = nlohmann::json::parse(candidate);
          return j.dump();
        } catch (...) {
        }
      }
      // 尝试自动补全未闭合的数组
      size_t last_quote = input.rfind('"');
      if (last_quote != std::string::npos && last_quote > arr_start) {
        std::string candidate =
            input.substr(arr_start, last_quote - arr_start + 1) + "]";
        try {
          auto j = nlohmann::json::parse(candidate);
          return j.dump();
        } catch (...) {
        }
      }
    }

    // 4. 提取双引号包裹的实体词
    nlohmann::json extracted_array = nlohmann::json::array();
    size_t pos = 0;
    while ((pos = input.find('"', pos)) != std::string::npos) {
      size_t next_pos = input.find('"', pos + 1);
      if (next_pos == std::string::npos) break;
      std::string word = input.substr(pos + 1, next_pos - pos - 1);
      if (!word.empty() && word != "nouns" && word != "entities") {
        extracted_array.push_back(std::move(word));
      }
      pos = next_pos + 1;
    }
    if (!extracted_array.empty()) {
      return extracted_array.dump();
    }

    return fallback_json_;
  }

  std::string fallback_json_ = "{}";
  bool extract_json_block_ = true;

  BoundInput<TextBatch> in_text_;
  BoundOutput<StructuredDocumentBatch> out_doc_;
};

NodeDefinition MakeStructuredJsonParseNodeDefinition() {
  NodeDefinition def;
  def.node_type = StructuredJsonParseNode::kNodeType;
  def.category = "common";
  def.description = "Structured JSON parser and validator node";
  def.inputs = {
      RequiredInputPort("text", BlackboardKey<TextBatch>{"", "TextBatch"})};
  def.outputs = {OutputPort("document", BlackboardKey<StructuredDocumentBatch>{
                                            "", "StructuredDocumentBatch"})};
  def.config_fields = {
      ConfigFieldDefinition{"fallback_json", ConfigValueKind::kString, false,
                            "{}"},
      ConfigFieldDefinition{"extract_json_block", ConfigValueKind::kBoolean,
                            false, true}};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(StructuredJsonParseNode,
                              MakeStructuredJsonParseNodeDefinition());

}  // namespace alg_framework

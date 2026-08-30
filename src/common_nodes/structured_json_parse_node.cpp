#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

#include "company_alg_log.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "nodes/node_error_codes.h"
#include "nodes/node_support.h"

namespace alg_framework {

/**
 * @brief 结构化 JSON 解析与文本提取受控算子 (StructuredJsonParseNode)
 */
class StructuredJsonParseNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "StructuredJsonParseNode";

  StructuredJsonParseNode()
      : NodeBase(kNodeType), in_text_("text"), out_doc_("document") {}

 protected:
  bool InitNode(const NodeInitContext& init_ctx, const nlohmann::json& config,
                SessionContext& /*session_ctx*/) override {
    BindPort(init_ctx, in_text_);
    BindPort(init_ctx, out_doc_);

    fallback_json_ = config.value("fallback_json", "{}");
    extract_json_block_ = config.value("extract_json_block", true);
    failure_policy_ = config.value("failure_policy", "configured_fallback");
    if (failure_policy_ != "fail" && failure_policy_ != "emit_diagnostic" &&
        failure_policy_ != "configured_fallback") {
      return false;
    }

    required_fields_.clear();
    if (config.contains("required_fields")) {
      if (!config["required_fields"].is_array()) return false;
      for (const auto& f : config["required_fields"]) {
        if (!f.is_string() || f.get<std::string>().empty()) return false;
        required_fields_.push_back(f.get<std::string>());
      }
    }

    field_types_.clear();
    if (config.contains("field_types")) {
      if (!config["field_types"].is_object()) return false;
      for (auto it = config["field_types"].begin();
           it != config["field_types"].end(); ++it) {
        static const std::unordered_set<std::string> kSupportedTypes = {
            "string", "number", "boolean", "object", "array"};
        if (!it.value().is_string()) return false;
        const auto type_name = it.value().get<std::string>();
        if (!kSupportedTypes.count(type_name)) return false;
        field_types_[it.key()] = type_name;
      }
    }

    // 验证 fallback_json 是否为合法 JSON 并预解析
    try {
      fallback_structured_ = nlohmann::json::parse(fallback_json_);
    } catch (const std::exception& e) {
      ALG_LOG_ERROR("[StructuredJsonParseNode] Invalid fallback_json: %s\n",
                    e.what());
      return false;
    }
    if (failure_policy_ != "fail" &&
        !ValidateStructuredFields(fallback_structured_, nullptr)) {
      ALG_LOG_ERROR(
          "[StructuredJsonParseNode] fallback_json does not satisfy "
          "required_fields/field_types\n");
      return false;
    }
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* text_items = in_text_.Require(
        req_ctx, node_error::structured_json_parse::kMissingInput,
        "StructuredJsonParseNode input");
    if (!text_items) {
      return node_error::structured_json_parse::kMissingInput;
    }

    StructuredDocumentBatch output_docs;
    output_docs.reserve(text_items->size());

    for (const auto& item : *text_items) {
      std::string parsed_json_str;
      nlohmann::json parsed_structured = nlohmann::json::object();
      JsonParseStatus status = JsonParseStatus::kOk;
      std::string diag;

      bool ok = ParseOrExtractJson(item.data, &parsed_json_str,
                                   &parsed_structured, &status, &diag);
      if (ok && !ValidateStructuredFields(parsed_structured, &diag)) {
        ok = false;
        status = JsonParseStatus::kFailed;
      }

      if (!ok) {
        if (failure_policy_ == "fail") {
          return Fail(req_ctx, node_error::structured_json_parse::kParseFailed,
                      "JSON parse failed for sample: " + diag);
        } else if (failure_policy_ == "emit_diagnostic") {
          output_docs.emplace_back(
              item.req_id, item.sub_id,
              JsonDocumentItem(fallback_json_, false, JsonParseStatus::kFailed,
                               diag, fallback_structured_));
          continue;
        } else {  // configured_fallback
          output_docs.emplace_back(
              item.req_id, item.sub_id,
              JsonDocumentItem(fallback_json_, true,
                               JsonParseStatus::kFallbackApplied, diag,
                               fallback_structured_));
          continue;
        }
      }

      output_docs.emplace_back(
          item.req_id, item.sub_id,
          JsonDocumentItem(std::move(parsed_json_str), true, status, diag,
                           std::move(parsed_structured)));
    }

    out_doc_.Set(req_ctx, std::move(output_docs));
    return 0;
  }

 private:
  bool ValidateStructuredFields(const nlohmann::json& document,
                                std::string* diagnostic) const {
    for (const auto& field : required_fields_) {
      if (!document.is_object() || !document.contains(field)) {
        if (diagnostic) {
          *diagnostic = "Missing required structured field: " + field;
        }
        return false;
      }
    }
    for (const auto& [field, type] : field_types_) {
      if (!document.is_object() || !document.contains(field)) {
        if (diagnostic) *diagnostic = "Missing field for type check: " + field;
        return false;
      }
      const auto& value = document[field];
      const bool matches = (type == "string" && value.is_string()) ||
                           (type == "number" && value.is_number()) ||
                           (type == "boolean" && value.is_boolean()) ||
                           (type == "object" && value.is_object()) ||
                           (type == "array" && value.is_array());
      if (!matches) {
        if (diagnostic) {
          *diagnostic = "Field '" + field + "' type mismatch, expected " + type;
        }
        return false;
      }
    }
    return true;
  }

  bool ParseOrExtractJson(const std::string& input, std::string* out_json,
                          nlohmann::json* out_structured,
                          JsonParseStatus* out_status,
                          std::string* out_diag) const {
    if (input.empty()) {
      if (failure_policy_ == "fail") {
        *out_diag = "Empty input string";
        return false;
      }
      *out_json = fallback_json_;
      if (out_structured) *out_structured = fallback_structured_;
      *out_status = JsonParseStatus::kFallbackApplied;
      *out_diag = "Empty input string";
      return true;
    }

    // 1. 尝试直接完整解析
    try {
      auto j = nlohmann::json::parse(input);
      *out_json = j.dump();
      if (out_structured) *out_structured = j;
      *out_status = JsonParseStatus::kOk;
      return true;
    } catch (const std::exception& e) {
      *out_diag = e.what();
    }

    if (!extract_json_block_) {
      return false;
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
          *out_json = j.dump();
          if (out_structured) *out_structured = j;
          *out_status = JsonParseStatus::kExtractedFromMarkdown;
          return true;
        } catch (...) {
        }
      }
    }

    // 3. 尝试搜索最外层 { ... }
    size_t obj_start = input.find('{');
    size_t obj_end = input.rfind('}');
    if (obj_start != std::string::npos && obj_end != std::string::npos &&
        obj_end > obj_start) {
      std::string candidate = input.substr(obj_start, obj_end - obj_start + 1);
      try {
        auto j = nlohmann::json::parse(candidate);
        *out_json = j.dump();
        if (out_structured) *out_structured = j;
        *out_status = JsonParseStatus::kAutoClosed;
        return true;
      } catch (...) {
      }
    }

    // 4. 尝试搜索最外层 [ ... ]
    size_t arr_start = input.find('[');
    size_t arr_end = input.rfind(']');
    if (arr_start != std::string::npos) {
      if (arr_end != std::string::npos && arr_end > arr_start) {
        std::string candidate =
            input.substr(arr_start, arr_end - arr_start + 1);
        try {
          auto j = nlohmann::json::parse(candidate);
          *out_json = j.dump();
          if (out_structured) *out_structured = j;
          *out_status = JsonParseStatus::kAutoClosed;
          return true;
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
          *out_json = j.dump();
          if (out_structured) *out_structured = j;
          *out_status = JsonParseStatus::kAutoClosed;
          return true;
        } catch (...) {
        }
      }
    }

    // 5. 提取双引号包裹的实体词
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
      *out_json = extracted_array.dump();
      if (out_structured) *out_structured = extracted_array;
      *out_status = JsonParseStatus::kAutoClosed;
      return true;
    }

    return false;
  }

  std::string fallback_json_ = "{}";
  nlohmann::json fallback_structured_ = nlohmann::json::object();
  bool extract_json_block_ = true;
  std::string failure_policy_ = "configured_fallback";
  std::vector<std::string> required_fields_;
  std::unordered_map<std::string, std::string> field_types_;

  BoundInput<TextBatch> in_text_;
  BoundOutput<StructuredDocumentBatch> out_doc_;
};

NodeDefinition MakeStructuredJsonParseNodeDefinition() {
  NodeDefinition def;
  def.node_type = StructuredJsonParseNode::kNodeType;
  def.category = "common";
  def.description = "Structured JSON parser and validator node";
  def.inputs = {RequiredInputPort("text",
                                  BlackboardKey<TextBatch>{"", "TextBatch"},
                                  "1:1", "preserve", "request")};
  def.outputs = {OutputPort(
      "document",
      BlackboardKey<StructuredDocumentBatch>{"", "StructuredDocumentBatch"},
      false, "1:1", "preserve", "request")};
  def.config_fields = {
      ConfigFieldDefinition{"fallback_json", ConfigValueKind::kString, false,
                            "{}"},
      ConfigFieldDefinition{"extract_json_block", ConfigValueKind::kBoolean,
                            false, true},
      ConfigFieldDefinition{"required_fields", ConfigValueKind::kArray, false},
      ConfigFieldDefinition{"field_types", ConfigValueKind::kObject, false},
      ConfigFieldDefinition{
          "failure_policy",
          ConfigValueKind::kString,
          false,
          "configured_fallback",
          std::nullopt,
          std::nullopt,
          {"fail", "emit_diagnostic", "configured_fallback"}}};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(StructuredJsonParseNode,
                              MakeStructuredJsonParseNodeDefinition());

}  // namespace alg_framework

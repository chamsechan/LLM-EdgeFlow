#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

#include "contracts/config_schema_validation.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "edgeflow/log.h"
#include "nodes/node_base.h"
#include "nodes/node_error_codes.h"

namespace llm_edgeflow {
namespace {
constexpr char kDefaultFallbackJson[] = "{}";
constexpr bool kDefaultExtractJsonBlock = true;
constexpr char kDefaultFailurePolicy[] = "configured_fallback";

const std::vector<ConfigFieldDefinition>& StructuredJsonParseConfigFields() {
  static const std::vector<ConfigFieldDefinition> fields = {
      ConfigFieldDefinition{"fallback_json",
                            ConfigValueKind::kString,
                            false,
                            kDefaultFallbackJson,
                            std::nullopt,
                            std::nullopt,
                            {},
                            "失败时使用的 JSON 文本字符串，例如 "
                            "\"{\\\"name\\\":\\\"unknown\\\"}\"；非 fail "
                            "模式须满足配置的字段检查。"},
      ConfigFieldDefinition{"extract_json_block",
                            ConfigValueKind::kBoolean,
                            false,
                            kDefaultExtractJsonBlock,
                            std::nullopt,
                            std::nullopt,
                            {},
                            "允许从代码围栏或周围文本中提取完整 JSON "
                            "对象/数组；false 时要求整段输入为 JSON。"},
      ConfigFieldDefinition{"required_fields",
                            ConfigValueKind::kArray,
                            false,
                            nlohmann::json(),
                            std::nullopt,
                            std::nullopt,
                            {},
                            "必须存在的顶层字段名数组，例如 [\"name\", "
                            "\"score\"]；不使用 JSON Pointer 或点号路径。"},
      ConfigFieldDefinition{
          "field_types",
          ConfigValueKind::kObject,
          false,
          nlohmann::json(),
          std::nullopt,
          std::nullopt,
          {},
          "顶层字段名到类型的映射，例如 "
          "{\"name\":\"string\",\"score\":\"number\"}；还支持 "
          "boolean/object/array，列出的字段须存在。"},
      ConfigFieldDefinition{"failure_policy",
                            ConfigValueKind::kString,
                            false,
                            kDefaultFailurePolicy,
                            std::nullopt,
                            std::nullopt,
                            {"fail", "emit_diagnostic", "configured_fallback"},
                            "fail 中止处理；emit_diagnostic "
                            "输出失败状态和备用值；configured_fallback "
                            "输出备用值并标记已使用回退。"}};
  return fields;
}
}  // namespace

/**
 * @brief 结构化 JSON 解析与文本提取受控算子 (StructuredJsonParseNode)
 */
struct StructuredJsonOptions {
  bool Load(const nlohmann::json& config) {
    nlohmann::json normalized;
    if (!ValidateAndNormalizeFields(StructuredJsonParseConfigFields(), config,
                                    &normalized, nullptr)) {
      return false;
    }
    fallback_json_ =
        normalized.value<std::string>("fallback_json", kDefaultFallbackJson);
    extract_json_block_ =
        normalized.value<bool>("extract_json_block", kDefaultExtractJsonBlock);
    failure_policy_ =
        normalized.value<std::string>("failure_policy", kDefaultFailurePolicy);
    if (failure_policy_ != "fail" && failure_policy_ != "emit_diagnostic" &&
        failure_policy_ != "configured_fallback") {
      return false;
    }

    required_fields_.clear();
    if (normalized.contains("required_fields")) {
      if (!normalized["required_fields"].is_array()) return false;
      for (const auto& f : normalized["required_fields"]) {
        if (!f.is_string() || f.get<std::string>().empty()) return false;
        required_fields_.push_back(f.get<std::string>());
      }
    }

    field_types_.clear();
    if (normalized.contains("field_types")) {
      if (!normalized["field_types"].is_object()) return false;
      for (auto it = normalized["field_types"].begin();
           it != normalized["field_types"].end(); ++it) {
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

  std::string fallback_json_ = kDefaultFallbackJson;
  nlohmann::json fallback_structured_ = nlohmann::json::object();
  bool extract_json_block_ = kDefaultExtractJsonBlock;
  std::string failure_policy_ = kDefaultFailurePolicy;
  std::vector<std::string> required_fields_;
  std::unordered_map<std::string, std::string> field_types_;
};

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

    return options_.Load(config);
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
      if (ok && !options_.ValidateStructuredFields(parsed_structured, &diag)) {
        ok = false;
        status = JsonParseStatus::kFailed;
      }

      if (!ok) {
        if (options_.failure_policy_ == "fail") {
          return Fail(req_ctx, node_error::structured_json_parse::kParseFailed,
                      "JSON parse failed for sample: " + diag);
        } else if (options_.failure_policy_ == "emit_diagnostic") {
          output_docs.emplace_back(
              item.req_id, item.sub_id,
              JsonDocumentItem(options_.fallback_json_, false,
                               JsonParseStatus::kFailed, diag,
                               options_.fallback_structured_));
          continue;
        } else {  // configured_fallback
          output_docs.emplace_back(
              item.req_id, item.sub_id,
              JsonDocumentItem(options_.fallback_json_, true,
                               JsonParseStatus::kFallbackApplied, diag,
                               options_.fallback_structured_));
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
  bool ParseOrExtractJson(const std::string& input, std::string* out_json,
                          nlohmann::json* out_structured,
                          JsonParseStatus* out_status,
                          std::string* out_diag) const {
    if (input.empty()) {
      *out_diag = "Empty input string";
      return false;
    }

    const auto parse_candidate = [&](const std::string& candidate,
                                     JsonParseStatus status) {
      try {
        auto parsed = nlohmann::json::parse(candidate);
        *out_json = parsed.dump();
        if (out_structured) *out_structured = std::move(parsed);
        *out_status = status;
        out_diag->clear();
        return true;
      } catch (const std::exception& e) {
        *out_diag = e.what();
        return false;
      }
    };
    if (parse_candidate(input, JsonParseStatus::kOk)) return true;
    if (!options_.extract_json_block_) return false;
    const size_t code_block_start = input.find("```json");
    if (code_block_start != std::string::npos) {
      const size_t content_start = code_block_start + 7;
      const size_t code_block_end = input.find("```", content_start);
      if (code_block_end == std::string::npos) {
        *out_diag = "Unclosed JSON markdown block";
        return false;
      }
      // An invalid fenced value must not fall through to extracting its
      // children.
      return parse_candidate(
          input.substr(content_start, code_block_end - content_start),
          JsonParseStatus::kExtractedFromMarkdown);
    }

    // Preserve the first outer container, including arrays of objects. Never
    // extract a valid child from a malformed or truncated parent container.
    const size_t start = input.find_first_of("[{");
    if (start == std::string::npos) return false;
    std::vector<char> delimiters;
    bool in_string = false;
    bool escaped = false;
    for (size_t pos = start; pos < input.size(); ++pos) {
      const char ch = input[pos];
      if (in_string) {
        if (escaped) {
          escaped = false;
        } else if (ch == '\\') {
          escaped = true;
        } else if (ch == '"') {
          in_string = false;
        }
        continue;
      }
      if (ch == '"') {
        in_string = true;
      } else if (ch == '[' || ch == '{') {
        delimiters.push_back(ch);
      } else if (ch == ']' || ch == '}') {
        if (delimiters.empty() ||
            delimiters.back() != (ch == ']' ? '[' : '{')) {
          *out_diag = "Mismatched JSON container delimiters";
          return false;
        }
        delimiters.pop_back();
        if (delimiters.empty()) {
          return parse_candidate(input.substr(start, pos - start + 1),
                                 JsonParseStatus::kOk);
        }
      }
    }
    *out_diag = "Incomplete JSON container";
    return false;
  }

  StructuredJsonOptions options_;
  BoundInput<TextBatch> in_text_;
  BoundOutput<StructuredDocumentBatch> out_doc_;
};

NodeDefinition MakeStructuredJsonParseNodeDefinition() {
  NodeDefinition def;
  def.node_type = StructuredJsonParseNode::kNodeType;
  def.category = "common";
  def.validate_config = [](const nlohmann::json& config, const auto&,
                           std::string* diagnostic) {
    StructuredJsonOptions options;
    const bool ok = options.Load(config);
    if (!ok && diagnostic)
      *diagnostic = "Invalid structured JSON fields, types or fallback";
    return ok;
  };
  def.description = "Complete JSON parsing and block extraction without repair";
  def.inputs = {RequiredInputPort("text",
                                  BlackboardKey<TextBatch>{"", "TextBatch"},
                                  "1:1", "preserve", "request")};
  def.outputs = {OutputPort(
      "document",
      BlackboardKey<StructuredDocumentBatch>{"", "StructuredDocumentBatch"},
      "1:1", "preserve", "request")};
  def.config_fields = StructuredJsonParseConfigFields();
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(StructuredJsonParseNode,
                              MakeStructuredJsonParseNodeDefinition());

}  // namespace llm_edgeflow

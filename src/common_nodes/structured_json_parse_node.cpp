#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/common_contracts.h"
#include "edgeflow/log.h"
#include "nodes/authoring.h"
#include "nodes/node_error_codes.h"

namespace llm_edgeflow {
namespace {
/**
 * @brief 结构化 JSON 解析与文本提取受控算子 (StructuredJsonParseNode)
 */
struct Params {
  bool ValidateStructuredFields(const nlohmann::json& document,
                                std::string* diagnostic) const {
    for (const auto& field : required_fields) {
      if (!document.is_object() || !document.contains(field)) {
        if (diagnostic) {
          *diagnostic = "Missing required structured field: " + field;
        }
        return false;
      }
    }
    for (const auto& [field, type] : field_types) {
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

  nlohmann::json fallback = nlohmann::json::object();
  bool extract_json_block = true;
  std::vector<std::string> required_fields;
  std::map<std::string, std::string> field_types;
  std::string failure_policy;
  // 由 fallback 派生，不对应配置项：备用值的紧凑序列化。
  std::string fallback_text;
};

bool ValidateParams(const Params& params, std::string* diagnostic) {
  for (const auto& field : params.required_fields) {
    if (field.empty()) {
      if (diagnostic) *diagnostic = "required_fields entries must not be empty";
      return false;
    }
  }
  if (params.failure_policy != "fail") {
    std::string detail;
    if (!params.ValidateStructuredFields(params.fallback, &detail)) {
      if (diagnostic) {
        *diagnostic =
            "fallback does not satisfy required_fields/field_types: " + detail;
      }
      return false;
    }
  }
  return true;
}

struct Inputs {
  const TextBatch* text = nullptr;
};

bool ParseOrExtractJson(const Params& options, const std::string& input,
                        std::string* out_json, nlohmann::json* out_structured,
                        JsonParseStatus* out_status, std::string* out_diag) {
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
  if (!options.extract_json_block) return false;
  const size_t code_block_start = input.find("```json");
  if (code_block_start != std::string::npos) {
    const size_t content_start = code_block_start + 7;
    const size_t code_block_end = input.find("```", content_start);
    if (code_block_end == std::string::npos) {
      *out_diag = "Unclosed JSON markdown block";
      return false;
    }
    // 围栏内的值非法时，不得转而提取其子元素。
    return parse_candidate(
        input.substr(content_start, code_block_end - content_start),
        JsonParseStatus::kExtractedFromMarkdown);
  }

  // 保留第一个外层容器 (含对象数组)。绝不从格式错误或被截断的父容器中
  // 提取合法的子元素。
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
      if (delimiters.empty() || delimiters.back() != (ch == ']' ? '[' : '{')) {
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

NodeResult<StructuredDocumentBatch> Run(const Inputs& inputs,
                                        const Params& options) {
  const auto* text_items = inputs.text;
  StructuredDocumentBatch output_docs;
  output_docs.reserve(text_items->size());

  for (const auto& item : *text_items) {
    std::string parsed_json_str;
    nlohmann::json parsed_structured = nlohmann::json::object();
    JsonParseStatus status = JsonParseStatus::kOk;
    std::string diag;

    bool ok = ParseOrExtractJson(options, item.data, &parsed_json_str,
                                 &parsed_structured, &status, &diag);
    if (ok && !options.ValidateStructuredFields(parsed_structured, &diag)) {
      ok = false;
      status = JsonParseStatus::kFailed;
    }

    if (!ok) {
      if (options.failure_policy == "fail") {
        return NodeResult<StructuredDocumentBatch>::Failure(
            NodeErrorKind::kBusinessError,
            "JSON parse failed for sample: " + diag,
            node_error::structured_json_parse::kParseFailed);
      } else if (options.failure_policy == "emit_diagnostic") {
        output_docs.emplace_back(
            item.req_id, item.sub_id,
            JsonDocumentItem(options.fallback_text, false,
                             JsonParseStatus::kFailed, diag, options.fallback));
        continue;
      } else {  // configured_fallback
        output_docs.emplace_back(
            item.req_id, item.sub_id,
            JsonDocumentItem(options.fallback_text, true,
                             JsonParseStatus::kFallbackApplied, diag,
                             options.fallback));
        continue;
      }
    }

    output_docs.emplace_back(
        item.req_id, item.sub_id,
        JsonDocumentItem(std::move(parsed_json_str), true, status, diag,
                         std::move(parsed_structured)));
  }

  return NodeResult<StructuredDocumentBatch>::Success(std::move(output_docs));
}

auto Spec() {
  auto params = Parameters<Params>(
      {Field("fallback", &Params::fallback)
           .Default(nlohmann::json::object())
           .Description(
               "失败时使用的备用 JSON 值，例如 {\"name\":\"unknown\"}；"
               "非 fail 模式须满足配置的字段检查。"),
       Field("extract_json_block", &Params::extract_json_block)
           .Default(true)
           .Description("允许从代码围栏或周围文本中提取完整 JSON "
                        "对象/数组；false 时要求整段输入为 JSON。"),
       Field("required_fields", &Params::required_fields)
           .Default({})
           .Description("必须存在的顶层字段名数组，例如 [\"name\", "
                        "\"score\"]；不使用 JSON Pointer 或点号路径。"),
       Field("field_types", &Params::field_types)
           .Default({})
           .Enum({"string", "number", "boolean", "object", "array"})
           .Description(
               "顶层字段名到类型的映射，例如 "
               "{\"name\":\"string\",\"score\":\"number\"}；"
               "类型为 string/number/boolean/object/array，列出的字段须存在。"),
       Field("failure_policy", &Params::failure_policy)
           .Default("configured_fallback")
           .Enum({"fail", "emit_diagnostic", "configured_fallback"})
           .Description("fail 中止处理；emit_diagnostic "
                        "输出失败状态和备用值；configured_fallback "
                        "输出备用值并标记已使用回退。")});
  params
      .Prepare([](Params* value, std::string*) {
        value->fallback_text = value->fallback.dump();
        return true;
      })
      .Validate(&ValidateParams);
  return MakeNodeSpec(
             InputsOf<Inputs>({Required("text", &Inputs::text)}),
             PreservedOutput<StructuredDocumentBatch>("document", "text"),
             std::move(params), &Run)
      .Category("common")
      .Description("Complete JSON parsing and block extraction without repair")
      .ParallelSafe(true);
}
}  // namespace
REGISTER_FUNCTION_NODE(StructuredJsonParseNode, Spec());
}  // namespace llm_edgeflow

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/common_contracts.h"
#include "engine/text/utf8.h"
#include "nodes/authoring.h"
#include "nodes/node_error_codes.h"
#include "nodes/text_template.h"

namespace llm_edgeflow {
namespace {
struct Params {
  std::string template_str;
  std::string separator;
  int64_t max_length = 0;
  std::string overflow_policy;
  // 由 Prepare 编译，不对应配置项。
  std::vector<TextTemplateToken> compiled_tokens;
};

const std::unordered_set<std::string>& TemplateVariables() {
  static const std::unordered_set<std::string> kVariables = {
      "primary", "context", "matches", "document"};
  return kVariables;
}

// 模板变量只能是输入端口名；连线信息可用时，引用的输入必须已连接。
bool PrepareTemplate(Params* state, const BindingFacts& bindings,
                     std::string* diagnostic) {
  std::vector<TextTemplateToken> tokens;
  std::string error;
  if (!ParseTextTemplate(state->template_str, &tokens, &error)) {
    if (diagnostic) *diagnostic = std::move(error);
    return false;
  }
  for (const auto& token : tokens) {
    if (token.type != TextTemplateTokenType::kVariable) continue;
    if (!TemplateVariables().count(token.value)) {
      if (diagnostic)
        *diagnostic = "Unknown template placeholder: " + token.value +
                      "; variables must be input ports (primary, context, "
                      "matches, document)";
      return false;
    }
    if (bindings.has_bindings && !bindings.IsConnected(token.value)) {
      if (diagnostic)
        *diagnostic = "Template variable '" + token.value +
                      "' requires a connected input";
      return false;
    }
  }
  state->compiled_tokens = std::move(tokens);
  return true;
}

struct Inputs {
  const TextBatch* primary = nullptr;
  const RankedTextBatch* context = nullptr;
  const RuleMatchBatch* matches = nullptr;
  const OcrDocumentBatch* document = nullptr;
};

NodeResult<TextBatch> Run(const Inputs& inputs, const Params& state) {
  const auto* primary_items = inputs.primary;
  const auto* context_items = inputs.context;
  const auto* matches_items = inputs.matches;
  const auto* document_items = inputs.document;
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
  std::set<uint32_t> req_ids_from_aggregated;

  if (primary_items) {
    for (const auto& item : *primary_items) {
      record_sample(item.req_id, item.sub_id);
      primary_by_sample[{item.req_id, item.sub_id}] = item.data;
    }
  }

  if (context_items) {
    for (const auto& item : *context_items) {
      req_ids_from_aggregated.insert(item.req_id);
      context_by_req[item.req_id].push_back(item.data.text);
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

    std::string rendered;
    rendered.reserve(256);

    for (const auto& token : state.compiled_tokens) {
      if (token.type == TextTemplateTokenType::kLiteral) {
        rendered += token.value;
      } else {
        const std::string& var = token.value;
        const std::string* value = nullptr;
        if (var == "primary") {
          if (p_it != primary_by_sample.end()) value = &primary_str;
        } else if (var == "context") {
          // 已存在的聚合批次中可能没有本请求的结果。
          if (context_items) value = &context_str;
        } else if (var == "matches") {
          if (matches_items) value = &matches_str;
        } else if (var == "document") {
          if (document_items) value = &doc_str;
        }
        // 输入连接了、但某个请求没有数据时，该变量为空字符串。
        rendered += value ? *value : std::string();
      }
    }

    if (rendered.size() > static_cast<size_t>(state.max_length)) {
      if (state.overflow_policy == "fail") {
        return NodeResult<TextBatch>::Failure(
            NodeErrorKind::kBusinessError,
            "Rendered prompt exceeds max_length of " +
                std::to_string(state.max_length),
            node_error::text_template::kRenderedOutputTooLong);
      }
      std::vector<size_t> boundaries;
      size_t invalid_offset = 0;
      if (!utf8::BuildCodePointBoundaries(rendered, &boundaries,
                                          &invalid_offset)) {
        return NodeResult<TextBatch>::Failure(
            NodeErrorKind::kBusinessError,
            "Rendered prompt contains invalid UTF-8 at byte offset " +
                std::to_string(invalid_offset),
            node_error::text_template::kInvalidUtf8);
      }
      const auto boundary =
          std::upper_bound(boundaries.begin(), boundaries.end(),
                           static_cast<size_t>(state.max_length));
      rendered.resize(*(boundary - 1));
    }

    output_batch.emplace_back(req_id, sub_id, std::move(rendered));
  }

  return NodeResult<TextBatch>::Success(std::move(output_batch));
}

auto Spec() {
  auto parameters = Parameters<Params>(
      {Field("template", &Params::template_str)
           .Default("{{primary}}")
           .Description("文本模板，变量只能是已连接的输入端口名 "
                        "{{primary}}/{{context}}/{{matches}}/{{document}}；"
                        "例如 \"问题：{{primary}}\"。"),
       Field("separator", &Params::separator)
           .Default("\n")
           .Description(
               "同一请求聚合多个输入文本时使用的分隔字符串，例如换行。"),
       Field("max_length", &Params::max_length)
           .Default(65536)
           .Range(1, 1048576)
           .Description("每条渲染结果的 UTF-8 字节上限；超出后按 "
                        "overflow_policy 处理。"),
       Field("overflow_policy", &Params::overflow_policy)
           .Default("fail")
           .Enum({"fail", "truncate"})
           .Description("fail 拒绝超过 max_length 的结果；truncate 按 "
                        "UTF-8 字符边界截断到字节上限内。")});
  parameters.Prepare(&PrepareTemplate);
  return MakeNodeSpec(
             InputsOf<Inputs>{OptionalValue("primary", &Inputs::primary),
                              OptionalValue("context", &Inputs::context,
                                            InputFlow::AggregateByRequest),
                              OptionalValue("matches", &Inputs::matches,
                                            InputFlow::AggregateByRequest),
                              OptionalValue("document", &Inputs::document,
                                            InputFlow::AggregateByRequest)},
             ProducedBatch<TextBatch>("text", PortFlow{}),
             std::move(parameters), Run)
      .Category("common")
      .Description(
          "Text template rendering: {{name}} substitutes variables; "
          "single {name} and JSON braces remain literal")
      .PortConstraints(
          {PortGroupConstraint(PortConstraintKind::kAtLeastOneOf,
                               {"primary", "context", "matches", "document"},
                               "TextTemplateNode requires at least one dynamic "
                               "input port to be bound")})
      .ParallelSafe(true)
      .WithControls(
          {ReplaceFields(kControlCmdUpdatePrompt, "update_prompt", {"template"},
                         "Update template string dynamically")});
}
}  // namespace

REGISTER_FUNCTION_NODE(TextTemplateNode, Spec());

}  // namespace llm_edgeflow

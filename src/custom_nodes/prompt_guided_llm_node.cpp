#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "contracts/traceable_item.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_definition.h"
#include "core/node_registry.h"
#include "edgeflow/log.h"
#include "engine/model_interface.h"
#include "nodes/model_bound_node.h"
#include "nodes/node_base.h"
#include "nodes/text_template.h"

namespace llm_edgeflow {
namespace custom_nodes {

namespace {
constexpr int kMissingInput = -8001;
constexpr int kModelInferenceFailed = -8002;
constexpr int kOutputProvenanceMismatch = -8003;

// Kept only for explicitly selected legacy syntax and unambiguous old
// templates.
bool ParseLegacyPromptTemplate(const std::string& pattern,
                               std::vector<TextTemplateToken>* parts,
                               std::string* error) {
  auto reject = [&](const std::string& message) {
    if (error) *error = message;
    return false;
  };
  parts->clear();
  std::string literal;
  for (size_t i = 0; i < pattern.size();) {
    const char c = pattern[i];
    if ((c == '{' || c == '}') && i + 1 < pattern.size() &&
        pattern[i + 1] == c) {
      literal += c;
      i += 2;
    } else if (c == '{') {
      const auto end = pattern.find('}', i + 1);
      if (end == std::string::npos)
        return reject("Unclosed prompt placeholder");
      const auto token = pattern.substr(i + 1, end - i - 1);
      if (token != "input" && token != "context") {
        return reject("Unknown prompt placeholder: " + token);
      }
      parts->push_back({TextTemplateTokenType::kLiteral, std::move(literal)});
      literal.clear();
      parts->push_back({TextTemplateTokenType::kVariable, token});
      i = end + 1;
    } else if (c == '}') {
      return reject(
          "Unescaped } in legacy prompt template; choose "
          "template_syntax=standard for literal JSON braces");
    } else {
      literal += c;
      ++i;
    }
  }
  parts->push_back({TextTemplateTokenType::kLiteral, std::move(literal)});
  return true;
}

// Parse only the original template. Request values are always opaque text.
bool ParsePromptConfig(const nlohmann::json& config,
                       std::vector<TextTemplateToken>* parts,
                       GenerateOptions* options, bool* uses_context,
                       std::string* error) {
  auto reject = [&](const std::string& message) {
    if (error) *error = message;
    return false;
  };
  try {
    if (config.contains("system_prompt")) {
      return reject(
          "system_prompt was renamed to prompt_prefix; it is ordinary input "
          "text, not a system message");
    }
    if (config.contains("fallback_text")) {
      return reject(
          "fallback_text is unsupported: model failures must remain failures");
    }
    parts->clear();
    *uses_context = false;
    const std::string pattern = config.value("prompt_template", "{input}");
    if (pattern.empty()) return reject("prompt_template must not be empty");
    const std::string syntax = config.value("template_syntax", "auto");
    if (syntax != "auto" && syntax != "standard" && syntax != "legacy") {
      return reject("template_syntax must be auto, standard or legacy");
    }
    if (syntax == "auto" && (pattern.find("{{") != std::string::npos ||
                             pattern.find("}}") != std::string::npos)) {
      return reject(
          "Ambiguous double braces in prompt_template: set "
          "template_syntax=standard to substitute {{input}}/{{context}} "
          "as in TextTemplateNode, or template_syntax=legacy to preserve "
          "old {{ / }} literal-brace escaping");
    }
    if (syntax == "standard") {
      if (!ParseTextTemplate(pattern, parts, error)) return false;
    } else if (!ParseLegacyPromptTemplate(pattern, parts, error)) {
      return false;
    }
    for (const auto& part : *parts) {
      if (part.type != TextTemplateTokenType::kVariable) continue;
      if (part.value != "input" && part.value != "context") {
        return reject("Unknown prompt placeholder: " + part.value);
      }
      *uses_context |= part.value == "context";
    }
    // Keep direct Node initialization as strict as native plan initialization.
    for (const char* field : {"max_tokens", "top_k"}) {
      if (config.contains(field) &&
          (!config[field].is_number_integer() ||
           config[field].get<double>() < 0 ||
           config[field].get<double>() > std::numeric_limits<int32_t>::max())) {
        return reject(std::string(field) + " must be a non-negative int32");
      }
    }
    *options = GenerateOptions{};
    options->temperature = config.value("temperature", 0.7f);
    options->max_tokens = config.value("max_tokens", 512);
    options->top_k = config.value("top_k", 0);
    options->top_p = config.value("top_p", 0.9f);
    options->repetition_penalty = config.value("repetition_penalty", 1.0f);
    if (options->max_tokens <= 0 || options->max_tokens > 32768 ||
        !std::isfinite(options->temperature) || options->temperature < 0 ||
        options->temperature > 2 || !std::isfinite(options->top_p) ||
        options->top_p < 1.0e-9f || options->top_p > 1 ||
        !std::isfinite(options->repetition_penalty) ||
        options->repetition_penalty < 1.0e-9f ||
        options->repetition_penalty > 100) {
      return reject("Generation options outside supported range");
    }
    if (config.contains("stop_words")) {
      if (!config["stop_words"].is_array())
        return reject("stop_words must be an array");
      for (const auto& word : config["stop_words"]) {
        if (!word.is_string() || word.get_ref<const std::string&>().empty()) {
          return reject("stop_words must contain non-empty strings");
        }
        options->stop_words.push_back(word.get<std::string>());
      }
    }
    (void)config.value("prompt_prefix", std::string{});
    (void)config.value("strip_markdown", false);
    return true;
  } catch (const std::exception& e) {
    return reject(e.what());
  }
}
}  // namespace

// Authoring example: local prompt processing, typed model call and response
// cleanup.
class PromptGuidedLlmNode final : public ModelBoundNode<ILlmModel> {
 public:
  inline static constexpr char kNodeType[] = "PromptGuidedLlmNode";

  explicit PromptGuidedLlmNode(std::string node_name = kNodeType)
      : ModelBoundNode<ILlmModel>(std::move(node_name)),
        in_port_("input"),
        context_port_("context"),
        out_port_("output") {}

 protected:
  bool InitModelNode(const NodeInitContext& init_ctx,
                     const nlohmann::json& config,
                     SessionContext& session_ctx) override {
    (void)session_ctx;
    BindPort(init_ctx, in_port_);
    BindPort(init_ctx, context_port_);
    BindPort(init_ctx, out_port_);

    std::string error;
    if (!ParsePromptConfig(config, &prompt_parts_, &gen_opt_, &uses_context_,
                           &error)) {
      ALG_LOG_ERROR("[PromptGuidedLlmNode] %s\n", error.c_str());
      return false;
    }
    if (uses_context_ && init_ctx.plan && !context_port_.IsBound())
      return false;
    prompt_prefix_ = config.value("prompt_prefix", "");
    strip_markdown_ = config.value("strip_markdown", false);
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* inputs = in_port_.Require(req_ctx, kMissingInput, "input text");
    if (!inputs) {
      return kMissingInput;
    }

    if (inputs->empty()) {
      out_port_.Set(req_ctx, TextBatch{});
      return 0;
    }
    const auto* contexts =
        uses_context_
            ? context_port_.Require(req_ctx, kMissingInput, "prompt context")
            : nullptr;
    if (uses_context_ && !contexts) return kMissingInput;

    // 组装模型输入批次，严格保留来源 (req_id, sub_id)
    TextBatch prompts;
    prompts.reserve(inputs->size());

    for (size_t i = 0; i < inputs->size(); ++i) {
      const auto& item = (*inputs)[i];
      std::string ctx_str;
      if (contexts) {
        for (const auto& c : *contexts) {
          if (c.req_id == item.req_id) {
            if (!ctx_str.empty()) ctx_str += "\n";
            ctx_str += c.data;
          }
        }
      }

      std::string rendered = RenderPrompt(item.data, ctx_str);
      prompts.emplace_back(item.req_id, item.sub_id, std::move(rendered));
    }

    TextBatch raw_outputs;
    int infer_ret = model()->Generate(prompts, gen_opt_, &raw_outputs);
    if (infer_ret != 0) {
      req_ctx.SetError(kModelInferenceFailed,
                       Name() + ": model inference failed with code " +
                           std::to_string(infer_ret));
      return kModelInferenceFailed;
    }

    if (raw_outputs.size() != prompts.size()) {
      req_ctx.SetError(kOutputProvenanceMismatch,
                       Name() + ": model output count mismatch (expected " +
                           std::to_string(prompts.size()) + ", got " +
                           std::to_string(raw_outputs.size()) + ")");
      return kOutputProvenanceMismatch;
    }

    TextBatch final_outputs;
    final_outputs.reserve(raw_outputs.size());
    for (size_t i = 0; i < raw_outputs.size(); ++i) {
      const auto& p = prompts[i];
      const auto& r = raw_outputs[i];
      if (r.req_id != p.req_id || r.sub_id != p.sub_id) {
        req_ctx.SetError(kOutputProvenanceMismatch,
                         Name() +
                             ": model output provenance mismatch for item " +
                             std::to_string(i));
        return kOutputProvenanceMismatch;
      }

      std::string out_str = r.data;
      if (strip_markdown_) {
        out_str = StripMarkdown(out_str);
      }
      final_outputs.emplace_back(r.req_id, r.sub_id, std::move(out_str));
    }

    out_port_.Set(req_ctx, std::move(final_outputs));
    return 0;
  }

 private:
  std::string RenderPrompt(const std::string& input,
                           const std::string& context) const {
    std::string result;
    if (!prompt_prefix_.empty()) {
      result += prompt_prefix_ + "\n";
    }
    for (const auto& part : prompt_parts_) {
      if (part.type == TextTemplateTokenType::kLiteral) {
        result += part.value;
      } else {
        result += part.value == "input" ? input : context;
      }
    }
    return result;
  }

  static bool StartsWith(std::string_view str, std::string_view prefix) {
    return str.size() >= prefix.size() &&
           str.compare(0, prefix.size(), prefix) == 0;
  }

  static bool EndsWith(std::string_view str, std::string_view suffix) {
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

  static std::string StripMarkdown(std::string_view text) {
    size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return "";
    size_t end = text.find_last_not_of(" \t\r\n");
    std::string_view trimmed = text.substr(start, end - start + 1);

    if (StartsWith(trimmed, "```")) {
      size_t first_nl = trimmed.find('\n');
      if (first_nl != std::string_view::npos) {
        trimmed = trimmed.substr(first_nl + 1);
      } else {
        trimmed = "";
      }
    }
    if (EndsWith(trimmed, "```")) {
      trimmed.remove_suffix(3);
    }
    start = trimmed.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return "";
    end = trimmed.find_last_not_of(" \t\r\n");
    return std::string(trimmed.substr(start, end - start + 1));
  }

  BoundInput<TextBatch> in_port_;
  BoundInput<TextBatch> context_port_;
  BoundOutput<TextBatch> out_port_;

  std::vector<TextTemplateToken> prompt_parts_;
  bool uses_context_ = false;
  std::string prompt_prefix_;
  bool strip_markdown_ = false;
  GenerateOptions gen_opt_;
};

NodeDefinition MakePromptGuidedLlmNodeDefinition() {
  NodeDefinition def;
  def.node_type = PromptGuidedLlmNode::kNodeType;
  def.category = "custom";
  def.description =
      "Custom domain node combining prompt construction, LLM generation, "
      "and response post-processing; template_syntax=standard uses "
      "{{name}}/{name} like TextTemplateNode, auto rejects ambiguous double "
      "braces, legacy preserves old brace escaping";
  def.inputs = {
      RequiredInputPort("input", BlackboardKey<TextBatch>{"", "TextBatch"},
                        "1:1", "preserve", "request"),
      OptionalInputPort("context", BlackboardKey<TextBatch>{"", "TextBatch"},
                        "N:1", "aggregate", "request"),
  };
  def.outputs = {
      OutputPort("output", BlackboardKey<TextBatch>{"", "TextBatch"}, "1:1",
                 "preserve", "request"),
  };
  def.config_fields = {
      ConfigFieldDefinition{
          "bind_model",
          ConfigValueKind::kString,
          false,
          "llm_model_v1",
          std::nullopt,
          std::nullopt,
          {},
          "引用 models[].model_id；所选模型必须提供 llm 文本生成能力。"},
      ConfigFieldDefinition{"prompt_template",
                            ConfigValueKind::kString,
                            false,
                            "{input}",
                            std::nullopt,
                            std::nullopt,
                            {},
                            "提示词模板；standard 示例为 "
                            "\"回答：{{input}}\\n背景：{{context}}\"，使用 "
                            "context 时须连接该输入。"},
      ConfigFieldDefinition{"template_syntax",
                            ConfigValueKind::kString,
                            false,
                            "auto",
                            std::nullopt,
                            std::nullopt,
                            {"auto", "standard", "legacy"},
                            "standard 使用 {{input}}/{{context}}；legacy 使用 "
                            "{input}/{context}；auto 拒绝有歧义的混用。"},
      ConfigFieldDefinition{"prompt_prefix",
                            ConfigValueKind::kString,
                            false,
                            "",
                            std::nullopt,
                            std::nullopt,
                            {},
                            "在渲染模板前追加的普通文本及换行；模型的 system "
                            "角色请使用 model_config.system_prompt。"},
      ConfigFieldDefinition{
          "temperature",
          ConfigValueKind::kNumber,
          false,
          0.7,
          0.0,
          2.0,
          {},
          "生成采样温度；0 用于贪心生成，具体采样由绑定模型执行。"},
      ConfigFieldDefinition{"max_tokens",
                            ConfigValueKind::kInteger,
                            false,
                            512,
                            1.0,
                            32768.0,
                            {},
                            "每条输入最多生成的 token "
                            "数，不包含输入提示词；还受模型上下文容量限制。"},
      ConfigFieldDefinition{
          "top_k",
          ConfigValueKind::kInteger,
          false,
          0,
          0.0,
          static_cast<double>(std::numeric_limits<int32_t>::max()),
          {},
          "采样时保留的候选 token 数；0 "
          "表示不按数量截断，区别于检索返回条数。"},
      ConfigFieldDefinition{"top_p",
                            ConfigValueKind::kNumber,
                            false,
                            0.9,
                            1.0e-9,
                            1.0,
                            {},
                            "核采样的累计概率阈值；1 表示不按累计概率截断。"},
      ConfigFieldDefinition{
          "repetition_penalty",
          ConfigValueKind::kNumber,
          false,
          1.0,
          1.0e-9,
          100.0,
          {},
          "已出现 token 的重复惩罚系数；1 不调整，大于 1 抑制重复。"},
      ConfigFieldDefinition{"strip_markdown",
                            ConfigValueKind::kBoolean,
                            false,
                            false,
                            std::nullopt,
                            std::nullopt,
                            {},
                            "移除模型输出两端空白和外层 Markdown "
                            "代码围栏，保留围栏内的文本内容。"},
      ConfigFieldDefinition{"stop_words",
                            ConfigValueKind::kArray,
                            false,
                            nlohmann::json::array(),
                            std::nullopt,
                            std::nullopt,
                            {},
                            "生成停止文本数组，例如 [\"结束\", "
                            "\"<END>\"]；命中后输出不包含停止文本。"}};
  def.validate_config = [](const nlohmann::json& config,
                           const std::unordered_set<std::string>& inputs,
                           std::string* error) {
    std::vector<TextTemplateToken> parts;
    GenerateOptions options;
    bool uses_context = false;
    if (!ParsePromptConfig(config, &parts, &options, &uses_context, error))
      return false;
    if (uses_context && inputs.count("context") == 0) {
      if (error)
        *error = "prompt_template uses context but context is not connected";
      return false;
    }
    return true;
  };
  def.model_capability = "llm";
  def.model_config_field = "bind_model";
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(PromptGuidedLlmNode,
                              MakePromptGuidedLlmNodeDefinition());

}  // namespace custom_nodes
}  // namespace llm_edgeflow

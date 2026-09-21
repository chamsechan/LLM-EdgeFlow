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
#include "nodes/node_config_parser.h"
#include "nodes/text_template.h"

namespace llm_edgeflow {
namespace custom_nodes {

namespace {
constexpr int kMissingInput = -8001;
constexpr int kModelInferenceFailed = -8002;
constexpr int kOutputProvenanceMismatch = -8003;

// Ordinary, owned configuration used by processing after initialization.
struct PromptConfig {
  std::vector<TextTemplateToken> prompt_parts;
  bool uses_context = false;
  std::string prompt_prefix;
  bool strip_markdown = false;
  GenerateOptions generation;
};

// Fields have already been validated and defaulted. Keep only this Node's
// semantic conversion here; request values never enter configuration parsing.
bool ParsePromptConfig(const nlohmann::json& config, PromptConfig* parameters,
                       std::string* error) {
  auto reject = [&](const std::string& message) {
    if (error) *error = message;
    return false;
  };
  auto& parts = parameters->prompt_parts;
  const auto& pattern =
      config.at("prompt_template").get_ref<const std::string&>();
  if (pattern.empty()) return reject("prompt_template must not be empty");
  if (!ParseTextTemplate(pattern, &parts, error)) return false;
  for (const auto& part : parts) {
    if (part.type != TextTemplateTokenType::kVariable) continue;
    if (part.value != "input" && part.value != "context") {
      return reject("Unknown prompt placeholder: " + part.value);
    }
    parameters->uses_context |= part.value == "context";
  }

  auto& options = parameters->generation;
  config.at("temperature").get_to(options.temperature);
  config.at("max_tokens").get_to(options.max_tokens);
  config.at("top_k").get_to(options.top_k);
  config.at("top_p").get_to(options.top_p);
  config.at("repetition_penalty").get_to(options.repetition_penalty);
  for (const auto& word : config.at("stop_words")) {
    if (!word.is_string() || word.get_ref<const std::string&>().empty()) {
      return reject("stop_words must contain non-empty strings");
    }
    options.stop_words.push_back(word.get<std::string>());
  }
  config.at("prompt_prefix").get_to(parameters->prompt_prefix);
  config.at("strip_markdown").get_to(parameters->strip_markdown);
  return true;
}

const NodeConfigParser<PromptConfig>& PromptConfiguration() {
  static const NodeConfigParser<PromptConfig> parser(
      {ConfigFieldDefinition{
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
                             "{{input}}",
                             std::nullopt,
                             std::nullopt,
                             {},
                             "提示词模板；使用 {{input}}/{{context}}，使用 "
                             "context 时须连接该输入。"},
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
                             "\"<END>\"]；命中后输出不包含停止文本。"}},
      ParsePromptConfig);
  return parser;
}

// Pure algorithm: renders prompt template with optional prefix and variables.
std::string RenderPromptFromParts(const std::string& prefix,
                                  const std::vector<TextTemplateToken>& parts,
                                  const std::string& input,
                                  const std::string& context) {
  std::string result;
  if (!prefix.empty()) {
    result += prefix + "\n";
  }
  for (const auto& part : parts) {
    if (part.type == TextTemplateTokenType::kLiteral) {
      result += part.value;
    } else {
      result += part.value == "input" ? input : context;
    }
  }
  return result;
}

// Pure algorithm: removes markdown code fences from LLM responses.
std::string StripMarkdownCodeFence(std::string_view text) {
  size_t start = text.find_first_not_of(" \t\r\n");
  if (start == std::string_view::npos) return "";
  size_t end = text.find_last_not_of(" \t\r\n");
  std::string_view trimmed = text.substr(start, end - start + 1);

  if (trimmed.size() >= 3 && trimmed.compare(0, 3, "```") == 0) {
    size_t first_nl = trimmed.find('\n');
    if (first_nl != std::string_view::npos) {
      trimmed = trimmed.substr(first_nl + 1);
    } else {
      trimmed = "";
    }
  }
  if (trimmed.size() >= 3 &&
      trimmed.compare(trimmed.size() - 3, 3, "```") == 0) {
    trimmed.remove_suffix(3);
  }
  start = trimmed.find_first_not_of(" \t\r\n");
  if (start == std::string_view::npos) return "";
  end = trimmed.find_last_not_of(" \t\r\n");
  return std::string(trimmed.substr(start, end - start + 1));
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
    auto parameters = PromptConfiguration().ParseNormalized(config, &error);
    if (!parameters) return init_ctx.Fail(error);
    if (parameters->uses_context && !context_port_.IsBound()) {
      return init_ctx.Fail(
          "prompt_template uses context but context is not connected");
    }
    config_ = std::move(*parameters);
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
        config_.uses_context
            ? context_port_.Require(req_ctx, kMissingInput, "prompt context")
            : nullptr;
    if (config_.uses_context && !contexts) return kMissingInput;

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
    std::string diagnostic;
    int infer_ret = model()->Generate(prompts, config_.generation, &raw_outputs,
                                      &diagnostic);
    if (infer_ret != 0) {
      req_ctx.SetError(kModelInferenceFailed,
                       Name() + ": model inference failed with code " +
                           std::to_string(infer_ret) +
                           (diagnostic.empty() ? "" : ": " + diagnostic));
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
      if (config_.strip_markdown) {
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
    return RenderPromptFromParts(config_.prompt_prefix, config_.prompt_parts,
                                 input, context);
  }

  static std::string StripMarkdown(std::string_view text) {
    return StripMarkdownCodeFence(text);
  }

  BoundInput<TextBatch> in_port_;
  BoundInput<TextBatch> context_port_;
  BoundOutput<TextBatch> out_port_;

  PromptConfig config_;
};

NodeDefinition MakePromptGuidedLlmNodeDefinition() {
  NodeDefinition def;
  def.node_type = PromptGuidedLlmNode::kNodeType;
  def.category = "custom";
  def.description =
      "Custom domain node combining prompt construction, LLM generation, "
      "and response post-processing using {{input}}/{{context}} templates";
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
  def.config_fields = PromptConfiguration().Fields();
  def.validate_config = [](const nlohmann::json& config,
                           const std::unordered_set<std::string>& inputs,
                           std::string* error) {
    const auto parameters =
        PromptConfiguration().ParseNormalized(config, error);
    if (!parameters) return false;
    if (parameters->uses_context && inputs.count("context") == 0) {
      if (error)
        *error = "prompt_template uses context but context is not connected";
      return false;
    }
    return true;
  };
  def.model_dependencies = {{"generator", "llm", "bind_model"}};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(PromptGuidedLlmNode,
                              MakePromptGuidedLlmNodeDefinition());

}  // namespace custom_nodes
}  // namespace llm_edgeflow

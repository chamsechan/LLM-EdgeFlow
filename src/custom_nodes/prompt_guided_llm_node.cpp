#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nodes/authoring.h"
#include "nodes/generate_options_config.h"
#include "nodes/text_template.h"

namespace llm_edgeflow {
namespace custom_nodes {

namespace {

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

  if (!ParseGenerateOptions(config, &parameters->generation, error))
    return false;
  config.at("prompt_prefix").get_to(parameters->prompt_prefix);
  config.at("strip_markdown").get_to(parameters->strip_markdown);
  return true;
}

std::vector<ConfigFieldDefinition> PromptConfigFields() {
  auto fields = GenerateOptionsFields(512);
  fields.insert(
      fields.begin(),
      {ConfigFieldDefinition{"prompt_template",
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
                             "角色请使用 model_config.system_prompt。"}});
  // Preserve the Catalog presentation order without assuming a field index.
  const auto stop_words = std::find_if(
      fields.begin(), fields.end(),
      [](const auto& field) { return field.name == "stop_words"; });
  fields.insert(stop_words,
                ConfigFieldDefinition{"strip_markdown",
                                      ConfigValueKind::kBoolean,
                                      false,
                                      false,
                                      std::nullopt,
                                      std::nullopt,
                                      {},
                                      "移除模型输出两端空白和外层 Markdown "
                                      "代码围栏，保留围栏内的文本内容。"});
  return fields;
}

const NodeConfigParser<PromptConfig>& PromptConfiguration() {
  static const NodeConfigParser<PromptConfig> parser(PromptConfigFields(),
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

struct Inputs {
  const TextBatch* input = nullptr;
  const TextBatch* context = nullptr;
};
struct Models {
  LlmCall generator;
};

NodeResult<TextBatch> GeneratePrompt(const Inputs& inputs,
                                     const PromptConfig& params,
                                     const Models& models) {
  if (inputs.input->empty()) return NodeResult<TextBatch>::Success({});
  if (params.uses_context && !inputs.context) {
    return NodeResult<TextBatch>::Failure(
        NodeErrorKind::kInputError, "missing prompt context",
        node_error::author_node::kMissingInput);
  }
  TextBatch prompts;
  prompts.reserve(inputs.input->size());
  for (const auto& item : *inputs.input) {
    std::string context;
    if (params.uses_context) {
      for (const auto& entry : *inputs.context) {
        if (entry.req_id != item.req_id) continue;
        if (!context.empty()) context += "\n";
        context += entry.data;
      }
    }
    prompts.emplace_back(
        item.req_id, item.sub_id,
        RenderPromptFromParts(params.prompt_prefix, params.prompt_parts,
                              item.data, context));
  }
  auto result = models.generator.Generate(prompts, params.generation);
  if (result.ok() && params.strip_markdown) {
    for (auto& item : result.value())
      item.data = StripMarkdownCodeFence(item.data);
  }
  return result;
}

auto PromptGuidedSpec() {
  auto params = Parameters<PromptConfig>{}.WithParser(PromptConfiguration());
  params.ValidateBindings([](const PromptConfig& config,
                             const std::unordered_set<std::string>& inputs,
                             std::string* error) {
    if (config.uses_context && inputs.count("context") == 0) {
      if (error)
        *error = "prompt_template uses context but context is not connected";
      return false;
    }
    return true;
  });
  return MakeBatchSpec(
             InputsOf<Inputs>{Required("input", &Inputs::input),
                              OptionalValue("context", &Inputs::context,
                                            InputFlow::AggregateByRequest)},
             PreservedOutput<TextBatch>("output", "input"), std::move(params),
             ModelsOf<Models>{Model("generator", "bind_model",
                                    &Models::generator,
                                    "引用 models[].model_id；所选模型必须提供 "
                                    "llm 文本生成能力。")},
             &GeneratePrompt)
      .Category("custom")
      .ParallelSafe(true)
      .Description(
          "Custom domain node combining prompt construction, LLM generation, "
          "and response post-processing using {{input}}/{{context}} templates");
}

REGISTER_FUNCTION_NODE(PromptGuidedLlmNode, PromptGuidedSpec());

}  // namespace
}  // namespace custom_nodes
}  // namespace llm_edgeflow

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

// 初始化后供处理阶段使用的普通自有配置。
struct Params {
  std::string prompt_template;
  std::string prompt_prefix;
  bool strip_markdown = false;
  GenerateOptions generation;
  // 由 prompt_template 在 Prepare 中解析得到。
  std::vector<TextTemplateToken> prompt_parts;
  bool uses_context = false;
};

// 字段已校验并填充默认值。此处只把模板转换为片段；请求值从不进入配置解析。
bool PreparePrompt(Params* params, std::string* error) {
  auto reject = [&](const std::string& message) {
    if (error) *error = message;
    return false;
  };
  if (params->prompt_template.empty())
    return reject("prompt_template must not be empty");
  params->prompt_parts.clear();
  params->uses_context = false;
  if (!ParseTextTemplate(params->prompt_template, &params->prompt_parts,
                         error)) {
    return false;
  }
  for (const auto& part : params->prompt_parts) {
    if (part.type != TextTemplateTokenType::kVariable) continue;
    if (part.value != "input" && part.value != "context") {
      return reject("Unknown prompt placeholder: " + part.value);
    }
    params->uses_context |= part.value == "context";
  }
  return true;
}

// 纯算法：使用可选前缀和变量渲染 prompt 模板。
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

// 纯算法：移除 LLM 响应中的 Markdown 代码围栏。
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

NodeResult<TextBatch> Run(const Inputs& inputs, const Params& params,
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

auto Spec() {
  auto params = GenerateParameters(
      512, &Params::generation,
      {Field("prompt_template", &Params::prompt_template)
           .Default("{{input}}")
           .Description("提示词模板；使用 {{input}}/{{context}}，使用 "
                        "context 时须连接该输入。"),
       Field("prompt_prefix", &Params::prompt_prefix)
           .Default("")
           .Description("在渲染模板前追加的普通文本及换行；模型的 system "
                        "角色请使用 model_config.system_prompt。"),
       Field("strip_markdown", &Params::strip_markdown)
           .Default(false)
           .Description("移除模型输出两端空白和外层 Markdown "
                        "代码围栏，保留围栏内的文本内容。")});
  params.Prepare(&PreparePrompt);
  params.ValidateBindings([](const Params& config,
                             const std::unordered_set<std::string>& inputs,
                             std::string* error) {
    if (config.uses_context && inputs.count("context") == 0) {
      if (error)
        *error = "prompt_template uses context but context is not connected";
      return false;
    }
    return true;
  });
  return MakeNodeSpec(
             InputsOf<Inputs>{Required("input", &Inputs::input),
                              OptionalValue("context", &Inputs::context,
                                            InputFlow::AggregateByRequest)},
             PreservedOutput<TextBatch>("output", "input"), std::move(params),
             ModelsOf<Models>{Model("generator", "bind_model",
                                    &Models::generator,
                                    "引用 models[].model_id；所选模型必须提供 "
                                    "llm 文本生成能力。")},
             &Run)
      .Category("custom")
      .ParallelSafe(true)
      .Description(
          "Custom domain node combining prompt construction, LLM generation, "
          "and response post-processing using {{input}}/{{context}} templates");
}

REGISTER_FUNCTION_NODE(PromptGuidedLlmNode, Spec());

}  // namespace
}  // namespace custom_nodes
}  // namespace llm_edgeflow

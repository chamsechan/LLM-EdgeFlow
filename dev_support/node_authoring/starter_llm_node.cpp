#include <string>

#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace custom_nodes {
namespace {

// LLM 入门模板：编写流程见 doc/dev_guide/first_custom_node.md。
// 从这里开始：这两个函数处理文本，而非平台结构。
static std::string BuildPrompt(const std::string& text) { return text; }

static std::string FormatAnswer(const std::string& text) { return text; }

struct Inputs {
  const TextBatch* input = nullptr;
};

struct Models {
  LlmCall generator;
};

// 逐条构造提示词，整批调用一次模型，再逐条整理回答；来源编号由框架保留。
// 生成参数（max_tokens、temperature 等）来自节点配置。
NodeResult<TextBatch> Run(const Inputs& inputs, const GenerateOptions& options,
                          const Models& models) {
  auto answers = models.generator.Generate(
      MapPayloads(*inputs.input, &BuildPrompt), options);
  if (!answers.ok()) return answers;
  return MapPayloads(answers.value(), &FormatAnswer);
}

auto Spec() {
  return MakeNodeSpec(InputsOf<Inputs>{Required("input", &Inputs::input)},
                      PreservedOutput<TextBatch>("output", "input"),
                      GenerateParameters(),
                      ModelsOf<Models>{
                          Model("generator", "bind_model", &Models::generator)},
                      &Run)
      .Description("LLM authoring starter");
}

REGISTER_FUNCTION_NODE(starter_llm, Spec());

}  // namespace
}  // namespace custom_nodes
}  // namespace llm_edgeflow

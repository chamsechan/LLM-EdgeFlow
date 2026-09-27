#include "nodes/authoring.h"
#include "nodes/generate_options_config.h"

namespace llm_edgeflow {
namespace {

struct Inputs {
  const TextBatch* prompt = nullptr;
};
struct Models {
  LlmCall generator;
};

NodeResult<TextBatch> Generate(const Inputs& inputs,
                               const GenerateOptions& params,
                               const Models& models) {
  return models.generator.Generate(*inputs.prompt, params);
}

auto LlmGenerateSpec() {
  NodeConfigParser<GenerateOptions> parser(GenerateOptionsFields(128),
                                           ParseGenerateOptions);
  return MakeBatchSpec(
             InputsOf<Inputs>{Required("prompt", &Inputs::prompt)},
             PreservedOutput<TextBatch>("text", "prompt"),
             Parameters<GenerateOptions>{}.WithParser(std::move(parser)),
             ModelsOf<Models>{Model("generator", "bind_model",
                                    &Models::generator,
                                    "引用 models[].model_id；所选模型必须提供 "
                                    "llm 文本生成能力。")},
             &Generate)
      .Category("common")
      .ParallelSafe(true)
      .Description("LLM generate text inference node");
}

REGISTER_FUNCTION_NODE(LlmGenerateNode, LlmGenerateSpec());

}  // namespace
}  // namespace llm_edgeflow

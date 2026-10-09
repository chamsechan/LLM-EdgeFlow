#include "nodes/authoring.h"
#include "nodes/generate_parameters.h"

namespace llm_edgeflow {
namespace {

struct Inputs {
  const TextBatch* prompt = nullptr;
};
struct Models {
  LlmCall generator;
};

NodeResult<TextBatch> Run(const Inputs& inputs, const GenerateOptions& params,
                          const Models& models) {
  return models.generator.Generate(*inputs.prompt, params);
}

auto Spec() {
  return MakeNodeSpec(InputsOf<Inputs>{Required("prompt", &Inputs::prompt)},
                      PreservedOutput<TextBatch>("text", "prompt"),
                      GenerateParameters(),
                      ModelsOf<Models>{
                          Model("generator", "bind_model", &Models::generator)},
                      &Run)
      .ValidateModels([](const GenerateOptions& params, const Models& models,
                         std::string* error) {
        if (params.random_seed < 0 || models.generator.SupportsRandomSeed())
          return true;
        if (error) *error = "Bound llm model does not support random_seed";
        return false;
      })
      .Category("common")
      .ParallelSafe(true)
      .Description("LLM generate text inference node");
}

REGISTER_FUNCTION_NODE(LlmGenerateNode, Spec());

}  // namespace
}  // namespace llm_edgeflow

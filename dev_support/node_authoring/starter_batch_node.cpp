#include <string>

#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace custom_nodes {
namespace starter_batch {

struct Inputs {
  const TextBatch* questions = nullptr;
  const TextBatch* context = nullptr;
};

struct Options {
  bool retry_once = false;
};

struct Models {
  LlmCall generator;
};

// All request values stay local to this ordinary business function.
NodeResult<TextBatch> Run(const Inputs& inputs, const Options& options,
                          const Models& models) {
  TextBatch prompts;
  prompts.reserve(inputs.questions->size());
  for (const auto& question : *inputs.questions) {
    std::string prompt;
    if (inputs.context) {
      for (const auto& context : *inputs.context) {
        if (context.req_id == question.req_id) prompt += context.data + "\n";
      }
    }
    prompts.emplace_back(question.req_id, question.sub_id,
                         prompt + question.data);
  }
  auto result = models.generator.Generate(prompts);
  if (!result.ok() && options.retry_once &&
      result.failure().kind == NodeErrorKind::kModelCallError) {
    return models.generator.Generate(prompts);
  }
  return result;
}

auto Spec() {
  return MakeBatchSpec(
             InputsOf<Inputs>({
                 Required("questions", &Inputs::questions),
                 Optional("context", &Inputs::context,
                          InputFlow::AggregateByRequest),
             }),
             PreservedOutput<TextBatch>("output", "questions"),
             Parameters<Options>({
                 Field("retry_once", &Options::retry_once).Default(false),
             }),
             ModelsOf<Models>({
                 Llm("generator", "bind_model", &Models::generator),
             }),
             &Run)
      .Description("Batch starter with optional request context and one retry");
}

REGISTER_FUNCTION_NODE(StarterBatchNode, Spec());

}  // namespace starter_batch
}  // namespace custom_nodes
}  // namespace llm_edgeflow

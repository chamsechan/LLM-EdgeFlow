#include <string>

#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace custom_nodes {
namespace starter_batch_join {

struct Inputs {
  const TextBatch* questions = nullptr;
  const TextBatch* attributes = nullptr;
};

struct Options {};

struct Models {
  LlmCall generator;
};

NodeResult<TextBatch> Run(const Inputs& inputs, const Options& /*options*/,
                          const Models& models) {
  if (!inputs.questions || inputs.questions->empty()) {
    return NodeResult<TextBatch>::Success(TextBatch{});
  }

  TextBatch empty_attrs;
  const auto& attrs = inputs.attributes ? *inputs.attributes : empty_attrs;
  auto joined = JoinByItem(*inputs.questions, attrs, JoinMode::kLeft);
  if (!joined.ok()) {
    return NodeResult<TextBatch>::Failure(std::move(joined).ExtractFailure());
  }

  TextBatch prompts;
  prompts.reserve(joined.value().size());
  for (const auto& row : joined.value()) {
    std::string prompt = row.left_payload();
    if (row.has_right()) {
      prompt += " [attr: " + *row.right_payload() + "]";
    }
    prompts.emplace_back(row.req_id(), row.sub_id(), std::move(prompt));
  }

  return models.generator.Generate(prompts);
}

auto Spec() {
  return MakeBatchSpec(InputsOf<Inputs>({
                           Required("questions", &Inputs::questions),
                           Optional("attributes", &Inputs::attributes),
                       }),
                       PreservedOutput<TextBatch>("output", "questions"),
                       Parameters<Options>({}),
                       ModelsOf<Models>({
                           Llm("generator", "bind_model", &Models::generator),
                       }),
                       &Run)
      .Description("Batch starter with traceable Left Join across inputs");
}

REGISTER_FUNCTION_NODE(StarterBatchJoinNode, Spec());

}  // namespace starter_batch_join
}  // namespace custom_nodes
}  // namespace llm_edgeflow

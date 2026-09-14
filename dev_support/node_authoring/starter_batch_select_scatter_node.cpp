#include <string>

#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace custom_nodes {
namespace starter_batch_select_scatter {

struct Inputs {
  const TextBatch* input = nullptr;
};

struct Options {
  std::string polish_tag = "[POLISH]";
};

struct Models {
  LlmCall generator;
  LlmCall polisher;
};

NodeResult<TextBatch> Run(const Inputs& inputs, const Options& options,
                          const Models& models) {
  if (!inputs.input || inputs.input->empty()) {
    return NodeResult<TextBatch>::Success(TextBatch{});
  }

  // Round 1: Generate initial drafts for all inputs
  auto first_res = models.generator.Generate(*inputs.input);
  if (!first_res.ok()) {
    return first_res;
  }

  const auto& drafts = first_res.value();

  // Step 2: Select items that require polishing (contain polish_tag)
  auto selection_res = SelectBatch(drafts, [&](const std::string& text) {
    return text.find(options.polish_tag) != std::string::npos;
  });
  if (!selection_res.ok()) {
    return NodeResult<TextBatch>::Failure(
        std::move(selection_res).ExtractFailure());
  }

  const auto& selection = selection_res.value();
  if (selection.empty()) {
    // Skip second model call when no items need polishing
    return first_res;
  }

  // Materialize owned sub-batch for the second model call
  TextBatch sub_batch = selection.Materialize();

  // Round 2: Call polisher model only on selected sub-batch
  auto second_res = models.polisher.Generate(sub_batch);
  if (!second_res.ok()) {
    return second_res;
  }

  // Step 3: Scatter-replace polished items back into full draft batch
  return ScatterReplace(selection, second_res.value());
}

auto Spec() {
  return MakeBatchSpec(
             InputsOf<Inputs>({
                 Required("input", &Inputs::input),
             }),
             PreservedOutput<TextBatch>("output", "input"),
             Parameters<Options>({
                 Field("polish_tag", &Options::polish_tag).Default("[POLISH]"),
             }),
             ModelsOf<Models>({
                 Llm("generator", "bind_model", &Models::generator),
                 Llm("polisher", "polish_model", &Models::polisher),
             }),
             &Run)
      .Description(
          "Batch starter with conditional sub-batch LLM refinement and "
          "scatter replacement");
}

REGISTER_FUNCTION_NODE(StarterBatchSelectScatterNode, Spec());

}  // namespace starter_batch_select_scatter
}  // namespace custom_nodes
}  // namespace llm_edgeflow

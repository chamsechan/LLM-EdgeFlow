#include <string>

#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace custom_nodes {
namespace starter_batch_group {

struct Inputs {
  const TextBatch* queries = nullptr;
  const TextBatch* references = nullptr;
};

struct Options {};

struct Models {
  LlmCall generator;
};

NodeResult<TextBatch> Run(const Inputs& inputs, const Options& /*options*/,
                          const Models& models) {
  if (!inputs.queries || inputs.queries->empty()) {
    return NodeResult<TextBatch>::Success(TextBatch{});
  }

  TextBatch empty_refs;
  const auto& refs = inputs.references ? *inputs.references : empty_refs;
  auto group_res = GroupByRequest(*inputs.queries, refs);
  if (!group_res.ok()) {
    return NodeResult<TextBatch>::Failure(
        std::move(group_res).ExtractFailure());
  }

  const auto& view = group_res.value();
  TextBatch prompts;
  prompts.reserve(inputs.queries->size());

  // Preserve anchor order for 1:1 PreservedOutput alignment
  for (size_t i = 0; i < inputs.queries->size(); ++i) {
    const auto& query_item = (*inputs.queries)[i];
    const auto& group = view.GroupByAnchorIndex(i);

    std::string context_text;
    for (const auto& ref_item : group.members()) {
      context_text += ref_item.get().data + "\n";
    }

    std::string full_prompt = context_text + query_item.data;
    prompts.emplace_back(query_item.req_id, query_item.sub_id,
                         std::move(full_prompt));
  }

  return models.generator.Generate(prompts);
}

auto Spec() {
  return MakeBatchSpec(InputsOf<Inputs>({
                           Required("queries", &Inputs::queries),
                           Optional("references", &Inputs::references,
                                    InputFlow::AggregateByRequest),
                       }),
                       PreservedOutput<TextBatch>("output", "queries"),
                       Parameters<Options>({}),
                       ModelsOf<Models>({
                           Llm("generator", "bind_model", &Models::generator),
                       }),
                       &Run)
      .Description(
          "Batch starter with traceable GroupByRequest reference aggregation");
}

REGISTER_FUNCTION_NODE(StarterBatchGroupNode, Spec());

}  // namespace starter_batch_group
}  // namespace custom_nodes
}  // namespace llm_edgeflow

#include <string>
#include <utility>

#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace custom_nodes {
namespace starter_multi_model {

struct Inputs {
  const TextBatch* questions = nullptr;
};

struct Models {
  LlmCall generator;
  EmbeddingCall encoder;
};

NodeResult<TextBatch> Run(const Inputs& inputs, const NoParameters&,
                          const Models& models) {
  auto embeddings = models.encoder.Embed(*inputs.questions);
  if (!embeddings.ok()) {
    return NodeResult<TextBatch>::Failure(
        std::move(embeddings).ExtractFailure());
  }
  TextBatch prompts;
  prompts.reserve(inputs.questions->size());
  for (std::size_t i = 0; i < inputs.questions->size(); ++i) {
    const auto& question = (*inputs.questions)[i];
    const auto& embedding = embeddings.value()[i].data;
    if (embedding.empty()) {
      return NodeResult<TextBatch>::Failure(NodeErrorKind::kBusinessError,
                                            "Question embedding is empty", 0,
                                            "prepare_prompt", "encoder");
    }
    prompts.emplace_back(question.req_id, question.sub_id,
                         question.data + "\nEmbedding dimensions: " +
                             std::to_string(embedding.size()));
  }
  return models.generator.Generate(prompts);
}

auto Spec() {
  return MakeBatchSpec(
             InputsOf<Inputs>({Required("questions", &Inputs::questions)}),
             PreservedOutput<TextBatch>("output", "questions"),
             ModelsOf<Models>({
                 Llm("generator", "bind_llm", &Models::generator),
                 Embedding("encoder", "bind_embedding", &Models::encoder),
             }),
             &Run)
      .Description(
          "Multi-model starter: embed questions then generate answers");
}

REGISTER_FUNCTION_NODE(StarterMultiModelNode, Spec());

}  // namespace starter_multi_model
}  // namespace custom_nodes
}  // namespace llm_edgeflow

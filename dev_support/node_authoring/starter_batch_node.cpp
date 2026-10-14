#include <string>

#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace custom_nodes {
namespace starter_batch {

struct Inputs {
  const TextBatch* questions = nullptr;
  const TextBatch* context = nullptr;
};

struct Models {
  LlmCall generator;
};

// 所有请求值都只在这个普通业务函数内使用。
NodeResult<TextBatch> Run(const Inputs& inputs, const Models& models) {
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
  return models.generator.Generate(prompts);
}

auto Spec() {
  return MakeNodeSpec(InputsOf<Inputs>({
                          Required("questions", &Inputs::questions),
                          Optional("context", &Inputs::context,
                                   InputFlow::AggregateByRequest),
                      }),
                      PreservedOutput<TextBatch>("output", "questions"),
                      ModelsOf<Models>({
                          Model("generator", "bind_model", &Models::generator),
                      }),
                      &Run)
      .Description("Batch starter with optional request context");
}

REGISTER_FUNCTION_NODE(starter_batch, Spec());

}  // namespace starter_batch
}  // namespace custom_nodes
}  // namespace llm_edgeflow

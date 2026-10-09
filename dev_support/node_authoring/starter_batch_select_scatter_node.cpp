#include <string>

#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace custom_nodes {
namespace starter_batch_select_scatter {

struct Inputs {
  const TextBatch* input = nullptr;
};

struct Params {
  std::string polish_tag = "[POLISH]";
};

struct Models {
  LlmCall generator;
  LlmCall polisher;
};

NodeResult<TextBatch> Run(const Inputs& inputs, const Params& params,
                          const Models& models) {
  if (!inputs.input || inputs.input->empty()) {
    return NodeResult<TextBatch>::Success(TextBatch{});
  }

  // 第 1 轮：为所有输入生成初稿
  auto first_res = models.generator.Generate(*inputs.input);
  if (!first_res.ok()) {
    return first_res;
  }

  const auto& drafts = first_res.value();

  // 第 2 步：选出需要润色的条目 (包含 polish_tag)
  auto selection_res = SelectBatch(drafts, [&](const std::string& text) {
    return text.find(params.polish_tag) != std::string::npos;
  });
  if (!selection_res.ok()) {
    return NodeResult<TextBatch>::Failure(
        std::move(selection_res).ExtractFailure());
  }

  const auto& selection = selection_res.value();
  if (selection.empty()) {
    // 没有条目需要润色时跳过第二次模型调用
    return first_res;
  }

  // 为第二次模型调用物化自有子批次
  TextBatch sub_batch = selection.Materialize();

  // 第 2 轮：只对选中的子批次调用润色模型
  auto second_res = models.polisher.Generate(sub_batch);
  if (!second_res.ok()) {
    return second_res;
  }

  // 第 3 步：把润色后的条目散回完整的初稿批次
  return ScatterReplace(selection, second_res.value());
}

auto Spec() {
  return MakeNodeSpec(
             InputsOf<Inputs>({
                 Required("input", &Inputs::input),
             }),
             PreservedOutput<TextBatch>("output", "input"),
             Parameters<Params>({
                 Field("polish_tag", &Params::polish_tag).Default("[POLISH]"),
             }),
             ModelsOf<Models>({
                 Model("generator", "bind_model", &Models::generator),
                 Model("polisher", "polish_model", &Models::polisher),
             }),
             &Run)
      .Description(
          "Batch starter with conditional sub-batch LLM refinement and "
          "scatter replacement");
}

REGISTER_FUNCTION_NODE(starter_batch_select_scatter, Spec());

}  // namespace starter_batch_select_scatter
}  // namespace custom_nodes
}  // namespace llm_edgeflow

#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace {
struct Inputs {
  const ImageFrameBatch* images = nullptr;
};
struct Outputs {
  OcrDocumentBatch document;
  TextBatch text;
};
struct Models {
  OcrCall detector;
};

NodeResult<Outputs> Run(const Inputs& inputs, const Models& models) {
  auto result = models.detector.Recognize(*inputs.images);
  if (!result.ok()) return NodeResult<Outputs>::Failure(result.failure());
  Outputs outputs;
  outputs.document = std::move(result).value();
  outputs.text.reserve(outputs.document.size());
  for (const auto& document : outputs.document) {
    outputs.text.emplace_back(document.req_id, document.sub_id,
                              document.data.combined_text);
  }
  return NodeResult<Outputs>::Success(std::move(outputs));
}

auto Spec() {
  return MakeNodeSpec(InputsOf<Inputs>{Required("images", &Inputs::images)},
                      OutputsOf<Outputs>(
                          {Produced("document", &Outputs::document, "images"),
                           Produced("text", &Outputs::text, "images")}),
                      ModelsOf<Models>{
                          Model("detector", "bind_model", &Models::detector)},
                      &Run)
      .Category("common")
      .ParallelSafe(true)
      .Description("OCR visual document detection and text recognition node");
}

REGISTER_FUNCTION_NODE(ocr_detect, Spec());
}  // namespace
}  // namespace llm_edgeflow

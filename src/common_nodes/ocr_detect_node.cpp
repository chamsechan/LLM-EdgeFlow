#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace {
struct Inputs {
  const ImageRefBatch* images = nullptr;
};
struct Outputs {
  OcrDocumentBatch document;
  TextBatch text;
};
struct Models {
  OcrCall detector;
};

NodeResult<Outputs> Recognize(const Inputs& inputs, const NoParameters&,
                              const Models& models) {
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

auto OcrDetectSpec() {
  return MakeBatchSpec(
             InputsOf<Inputs>{Required("images", &Inputs::images)},
             OutputsOf<Outputs>(
                 {Produced("document", &Outputs::document, "images"),
                  Produced("text", &Outputs::text, "images")}),
             Parameters<NoParameters>{},
             ModelsOf<Models>{Model("detector", "bind_model", &Models::detector,
                                    "引用 models[].model_id；所选模型必须提供 "
                                    "ocr 文档识别能力。")},
             &Recognize)
      .Category("common")
      .ParallelSafe(true)
      .Description("OCR visual document detection and text recognition node");
}

REGISTER_FUNCTION_NODE(OcrDetectNode, OcrDetectSpec());
}  // namespace
}  // namespace llm_edgeflow

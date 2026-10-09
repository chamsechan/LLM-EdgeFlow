#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace {
struct Inputs {
  const AudioPcmBatch* audio = nullptr;
};
struct Models {
  AsrCall transcriber;
};

NodeResult<TextBatch> Run(const Inputs& inputs, const Models& models) {
  return models.transcriber.Transcribe(*inputs.audio);
}

auto Spec() {
  return MakeNodeSpec(InputsOf<Inputs>{Required("audio", &Inputs::audio)},
                      PreservedOutput<TextBatch>("text", "audio"),
                      ModelsOf<Models>{Model("transcriber", "bind_model",
                                             &Models::transcriber)},
                      &Run)
      .Category("common")
      .ParallelSafe(true)
      .Description("Audio speech recognition (ASR) transcription node");
}

REGISTER_FUNCTION_NODE(AsrTranscribeNode, Spec());
}  // namespace
}  // namespace llm_edgeflow

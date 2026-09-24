#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace {
struct Inputs {
  const AudioPcmBatch* audio = nullptr;
};
struct Models {
  AsrCall transcriber;
};

NodeResult<TextBatch> Transcribe(const Inputs& inputs, const NoParameters&,
                                 const Models& models) {
  return models.transcriber.Transcribe(*inputs.audio);
}

auto AsrTranscribeSpec() {
  return MakeBatchSpec(
             InputsOf<Inputs>{Required("audio", &Inputs::audio)},
             PreservedOutput<TextBatch>("text", "audio"),
             Parameters<NoParameters>{},
             ModelsOf<Models>{Model(
                 "transcriber", "bind_model", &Models::transcriber,
                 "引用 models[].model_id；所选模型必须提供 asr 转写能力。")},
             &Transcribe)
      .Category("common")
      .ParallelSafe(true)
      .Description("Audio speech recognition (ASR) transcription node");
}

REGISTER_FUNCTION_NODE(AsrTranscribeNode, AsrTranscribeSpec());
}  // namespace
}  // namespace llm_edgeflow

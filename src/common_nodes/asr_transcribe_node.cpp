#include <string>
#include <utility>

#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace {
struct Inputs {
  const AudioPcmBatch* audio = nullptr;
};
struct Models {
  AsrCall transcriber;
};

NodeResult<TextBatch> Run(const Inputs& inputs, const TranscribeOptions& params,
                          const Models& models) {
  return models.transcriber.Transcribe(*inputs.audio, params);
}

auto Spec() {
  const TranscribeOptions defaults;
  auto params = Parameters<TranscribeOptions>(
      {Field("language", &TranscribeOptions::language)
           .Default(defaults.language)
           .Description("转写语言，例如 zh、en；auto "
                        "为自动识别。所绑模型须支持该语言。")});
  params.Validate([](const TranscribeOptions& params, std::string* error) {
    if (!params.language.empty()) return true;
    if (error) *error = "language must not be empty";
    return false;
  });
  return MakeNodeSpec(InputsOf<Inputs>{Required("audio", &Inputs::audio)},
                      PreservedOutput<TextBatch>("text", "audio"),
                      std::move(params),
                      ModelsOf<Models>{Model("transcriber", "bind_model",
                                             &Models::transcriber)},
                      &Run)
      .ValidateModels([](const TranscribeOptions& params, const Models& models,
                         std::string* error) {
        if (models.transcriber.SupportsLanguage(params.language)) return true;
        if (error)
          *error =
              "Bound ASR model does not support language: " + params.language;
        return false;
      })
      .Category("common")
      .ParallelSafe(true)
      .Description("Audio speech recognition (ASR) transcription node");
}

REGISTER_FUNCTION_NODE(asr_transcribe, Spec());
}  // namespace
}  // namespace llm_edgeflow

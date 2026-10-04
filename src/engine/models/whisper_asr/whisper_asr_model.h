#pragma once

#include <memory>
#include <string>

#include "engine/backend_interface.h"
#include "engine/model_identity.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {

class WhisperAsrModel final : public ModelIdentity<WhisperAsrModel, IAsrModel> {
 public:
  inline static constexpr char kModelType[] = "whisper_asr";
  static constexpr InferenceConcurrency kConcurrency =
      InferenceConcurrency::kConcurrent;
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic);

  int Transcribe(const AudioPcmBatch& audio, TextBatch* outputs,
                 std::string* diagnostic = nullptr) noexcept override;

 private:
  std::shared_ptr<IAudioTranscriptionSession> session_;
  int max_audio_seconds_ = 30;
  AudioTranscriptionOptions options_;
};

}  // namespace llm_edgeflow

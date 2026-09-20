#pragma once

#include <memory>
#include <string>

#include "engine/backend_interface.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {

class WhisperAsrModel final : public IAsrModel {
 public:
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic);

  const std::string& ModelType() const noexcept override;
  const std::string& Capability() const noexcept override;
  InferenceConcurrency Concurrency() const noexcept override;

  int Transcribe(const AudioPcmBatch& audio,
                 TextBatch* outputs) noexcept override;

 private:
  std::shared_ptr<IAudioTranscriptionSession> session_;
  int max_audio_seconds_ = 30;
  AudioTranscriptionOptions options_;
};

}  // namespace llm_edgeflow

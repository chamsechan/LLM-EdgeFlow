#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "engine/backend_interface.h"
#include "engine/model_identity.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {

class WhisperAsrModel final : public ModelIdentity<WhisperAsrModel, IAsrModel> {
 public:
  inline static constexpr char kImplName[] = "whisper_asr";
  static constexpr InferenceConcurrency kConcurrency =
      InferenceConcurrency::kConcurrent;
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic);

  bool SupportsLanguage(std::string_view language) const noexcept override;
  int Transcribe(const AudioPcmBatch& audio, const TranscribeOptions& options,
                 TextBatch* outputs,
                 std::string* diagnostic = nullptr) noexcept override;

 private:
  std::shared_ptr<IAudioTranscriptionSession> session_;
  int max_audio_seconds_ = 30;
  size_t max_output_bytes_ = 65536;
};

}  // namespace llm_edgeflow

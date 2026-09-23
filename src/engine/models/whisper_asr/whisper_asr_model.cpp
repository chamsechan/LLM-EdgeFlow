#include "engine/models/whisper_asr/whisper_asr_model.h"

#include <cmath>
#include <exception>
#include <string_view>
#include <vector>

#include "contracts/diagnostic.h"
#include "edgeflow/log.h"
#include "engine/fixed_batch_executor.h"
#include "engine/text/utf8.h"

namespace llm_edgeflow {
namespace {

inline std::string TrimAscii(std::string_view text) {
  const auto start = text.find_first_not_of(" \t\r\n");
  if (start == std::string_view::npos) return "";
  const auto end = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(start, end - start + 1));
}

}  // namespace

std::shared_ptr<IModel> WhisperAsrModel::Create(
    const ModelCreateContext& context, std::string* diagnostic) {
  try {
    auto session = std::dynamic_pointer_cast<IAudioTranscriptionSession>(
        context.backend_session);
    if (!session ||
        session->Protocol() != ExecutionProtocol::kAudioTranscription ||
        session->GetBatchPolicy().max_batch_size != 1 ||
        session->GetBatchPolicy().fixed_batch_size != 0) {
      throw std::runtime_error(
          "whisper_asr requires an IAudioTranscriptionSession with batch "
          "policy {1, 0}");
    }
    const std::string language =
        context.model_config.value("language", std::string("zh"));
    const int max_audio_seconds =
        context.model_config.value("max_audio_seconds", 30);
    const int max_output_bytes =
        context.model_config.value("max_output_bytes", 65536);

    if (language != "zh" && language != "en" && language != "auto") {
      throw std::runtime_error("Invalid language for whisper_asr: " + language);
    }
    if (max_audio_seconds < 1 || max_audio_seconds > 60) {
      throw std::runtime_error("max_audio_seconds must be between 1 and 60");
    }
    if (max_output_bytes < 1 || max_output_bytes > 65536) {
      throw std::runtime_error("max_output_bytes must be between 1 and 65536");
    }
    if (!session->SupportsLanguage(language)) {
      throw std::runtime_error("Backend session does not support language: " +
                               language);
    }

    auto model = std::make_shared<WhisperAsrModel>();
    model->session_ = std::move(session);
    model->max_audio_seconds_ = max_audio_seconds;
    model->options_.language = language;
    model->options_.max_output_bytes = static_cast<size_t>(max_output_bytes);
    return model;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(diagnostic, e.what());
    return nullptr;
  } catch (...) {
    SetDiagnosticNoexcept(diagnostic, "Unknown whisper_asr creation error");
    return nullptr;
  }
}

const std::string& WhisperAsrModel::ModelType() const noexcept {
  static const std::string value = "whisper_asr";
  return value;
}

const std::string& WhisperAsrModel::Capability() const noexcept {
  static const std::string value = "asr";
  return value;
}

InferenceConcurrency WhisperAsrModel::Concurrency() const noexcept {
  return InferenceConcurrency::kConcurrent;
}

int WhisperAsrModel::Transcribe(const AudioPcmBatch& audio, TextBatch* outputs,
                                std::string* diagnostic) noexcept {
  if (diagnostic) diagnostic->clear();
  if (!outputs) {
    SetDiagnosticNoexcept(diagnostic, "Model output pointer is null");
    return -1;
  }
  outputs->clear();
  if (!session_) {
    SetDiagnosticNoexcept(diagnostic, "Model session is null");
    return -1;
  }
  if (audio.empty()) return 0;

  try {
    const size_t max_samples = static_cast<size_t>(max_audio_seconds_) * 16000;
    for (const auto& item : audio) {
      if (item.data.sample_rate != 16000) {
        ALG_LOG_ERROR("[WhisperAsrModel] Audio sample rate %d != 16000\n",
                      item.data.sample_rate);
        SetDiagnosticNoexcept(diagnostic, "Audio sample rate must be 16000 Hz");
        return -1;
      }
      const size_t n_samples = item.data.pcm_data.size();
      if (n_samples == 0) {
        continue;
      }
      if (n_samples < 1600) {  // 100 ms
        ALG_LOG_ERROR(
            "[WhisperAsrModel] Audio sample count %zu < 1600 (100 ms)\n",
            n_samples);
        SetDiagnosticNoexcept(
            diagnostic, "Audio must contain at least 1600 samples (100 ms)");
        return -1;
      }
      if (n_samples > max_samples) {
        ALG_LOG_ERROR(
            "[WhisperAsrModel] Audio sample count %zu > max allowed %zu\n",
            n_samples, max_samples);
        SetDiagnosticNoexcept(diagnostic, "Audio exceeds model duration limit");
        return -1;
      }
      for (float s : item.data.pcm_data) {
        if (!std::isfinite(s) || s < -1.0f || s > 1.0f) {
          ALG_LOG_ERROR(
              "[WhisperAsrModel] Invalid audio sample amplitude (not finite or "
              "out of [-1, 1])\n");
          SetDiagnosticNoexcept(
              diagnostic, "Audio amplitude must be finite and within [-1, 1]");
          return -1;
        }
      }
    }

    return FixedBatchExecutor::ExecuteItems<AudioPcmPayload, std::string>(
        audio, session_->GetBatchPolicy(),
        [this, diagnostic](const TraceableItem<AudioPcmPayload>& input,
                           std::string* output) {
          const auto& item = input.data;
          if (item.pcm_data.empty()) {
            output->clear();
            return 0;
          }
          std::string raw_output;
          std::string reason;
          const int ret =
              session_->Transcribe(item, options_, &raw_output, &reason);
          if (ret != 0) {
            ALG_LOG_ERROR("[WhisperAsrModel] Transcription failed: %s\n",
                          reason.c_str());
            SetDiagnosticNoexcept(diagnostic, reason);
            return -1;
          }
          if (raw_output.find('\0') != std::string::npos) {
            ALG_LOG_ERROR(
                "[WhisperAsrModel] Output contains embedded NUL byte\n");
            SetDiagnosticNoexcept(diagnostic,
                                  "Transcription contains embedded NUL byte");
            return -1;
          }
          std::vector<size_t> boundaries;
          if (!utf8::BuildCodePointBoundaries(raw_output, &boundaries)) {
            ALG_LOG_ERROR("[WhisperAsrModel] Output is not valid UTF-8\n");
            SetDiagnosticNoexcept(diagnostic,
                                  "Transcription is not valid UTF-8");
            return -1;
          }
          std::string trimmed = TrimAscii(raw_output);
          if (trimmed.size() > options_.max_output_bytes) {
            ALG_LOG_ERROR(
                "[WhisperAsrModel] Output size %zu > max_output_bytes %zu\n",
                trimmed.size(), options_.max_output_bytes);
            SetDiagnosticNoexcept(diagnostic,
                                  "Transcription exceeds max_output_bytes");
            return -1;
          }
          *output = std::move(trimmed);
          return 0;
        },
        outputs, diagnostic);
  } catch (const std::exception& e) {
    ALG_LOG_ERROR("[WhisperAsrModel] Exception in Transcribe: %s\n", e.what());
    SetDiagnosticNoexcept(diagnostic, e.what());
    outputs->clear();
    return -1;
  } catch (...) {
    ALG_LOG_ERROR("[WhisperAsrModel] Unknown exception in Transcribe\n");
    SetDiagnosticNoexcept(diagnostic, "Unknown transcription exception");
    outputs->clear();
    return -1;
  }
}

static const ModelDefinition kWhisperAsrModelDefinition = [] {
  ModelDefinition definition;
  definition.model_type = "whisper_asr";
  definition.capability = "asr";
  definition.description =
      "Whisper automatic speech recognition model for float32 PCM";
  definition.required_protocol = ExecutionProtocol::kAudioTranscription;
  definition.concurrency = InferenceConcurrency::kConcurrent;
  definition.config_fields = {
      ConfigFieldDefinition{"language",
                            ConfigValueKind::kString,
                            false,
                            "zh",
                            std::nullopt,
                            std::nullopt,
                            {"zh", "en", "auto"},
                            "Target transcription language"},
      ConfigFieldDefinition{"max_audio_seconds",
                            ConfigValueKind::kInteger,
                            false,
                            30,
                            1.0,
                            60.0,
                            {},
                            "Maximum audio length in seconds"},
      ConfigFieldDefinition{"max_output_bytes",
                            ConfigValueKind::kInteger,
                            false,
                            65536,
                            1.0,
                            65536.0,
                            {},
                            "Maximum transcription output bytes"}};
  return definition;
}();

REGISTER_MODEL_WITH_DEFINITION(WhisperAsrModel, kWhisperAsrModelDefinition);

}  // namespace llm_edgeflow

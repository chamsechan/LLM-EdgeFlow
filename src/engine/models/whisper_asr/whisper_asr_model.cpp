#include "engine/models/whisper_asr/whisper_asr_model.h"

#include <cmath>
#include <exception>
#include <string_view>
#include <vector>

#include "company_alg_log.h"
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
    model->language_ = language;
    model->max_audio_seconds_ = max_audio_seconds;
    model->max_output_bytes_ = static_cast<size_t>(max_output_bytes);
    model->options_.language = language;
    model->options_.max_output_bytes = model->max_output_bytes_;
    return model;
  } catch (const std::exception& e) {
    inference_detail::SetDiagnostic(diagnostic, e.what());
    return nullptr;
  } catch (...) {
    inference_detail::SetDiagnostic(diagnostic,
                                    "Unknown whisper_asr creation error");
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

size_t WhisperAsrModel::GetMaxBatchSize() const noexcept { return 1; }

int WhisperAsrModel::Transcribe(const AudioPcmBatch& audio,
                                TextBatch* outputs) noexcept {
  if (!outputs) return -1;
  outputs->clear();
  if (!session_) return -1;
  if (audio.empty()) return 0;

  try {
    const size_t max_samples = static_cast<size_t>(max_audio_seconds_) * 16000;
    for (const auto& item : audio) {
      if (item.data.sample_rate != 16000) {
        ALG_LOG_ERROR("[WhisperAsrModel] Audio sample rate %d != 16000\n",
                      item.data.sample_rate);
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
        return -1;
      }
      if (n_samples > max_samples) {
        ALG_LOG_ERROR(
            "[WhisperAsrModel] Audio sample count %zu > max allowed %zu\n",
            n_samples, max_samples);
        return -1;
      }
      for (float s : item.data.pcm_data) {
        if (!std::isfinite(s) || s < -1.0f || s > 1.0f) {
          ALG_LOG_ERROR(
              "[WhisperAsrModel] Invalid audio sample amplitude (not finite or "
              "out of [-1, 1])\n");
          return -1;
        }
      }
    }

    return FixedBatchExecutor::Execute<AudioPcmPayload, std::string>(
        audio, session_->GetBatchPolicy(),
        [this, &audio](const BatchSlice& slice,
                       std::vector<std::string>* batch) {
          const auto& item = audio[slice.offset].data;
          if (item.pcm_data.empty()) {
            batch->push_back("");
            return 0;
          }
          std::string raw_output;
          std::string diagnostic;
          const int ret =
              session_->Transcribe(item, options_, &raw_output, &diagnostic);
          if (ret != 0) {
            ALG_LOG_ERROR("[WhisperAsrModel] Transcription failed: %s\n",
                          diagnostic.c_str());
            return -1;
          }
          if (raw_output.find('\0') != std::string::npos) {
            ALG_LOG_ERROR(
                "[WhisperAsrModel] Output contains embedded NUL byte\n");
            return -1;
          }
          std::vector<size_t> boundaries;
          if (!utf8::BuildCodePointBoundaries(raw_output, &boundaries)) {
            ALG_LOG_ERROR("[WhisperAsrModel] Output is not valid UTF-8\n");
            return -1;
          }
          std::string trimmed = TrimAscii(raw_output);
          if (trimmed.size() > max_output_bytes_) {
            ALG_LOG_ERROR(
                "[WhisperAsrModel] Output size %zu > max_output_bytes %zu\n",
                trimmed.size(), max_output_bytes_);
            return -1;
          }
          batch->push_back(std::move(trimmed));
          return 0;
        },
        outputs);
  } catch (const std::exception& e) {
    ALG_LOG_ERROR("[WhisperAsrModel] Exception in Transcribe: %s\n", e.what());
    outputs->clear();
    return -1;
  } catch (...) {
    ALG_LOG_ERROR("[WhisperAsrModel] Unknown exception in Transcribe\n");
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

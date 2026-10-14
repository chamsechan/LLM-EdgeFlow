#include "engine/models/whisper_asr/whisper_asr_model.h"

#include <cmath>
#include <exception>
#include <string_view>
#include <vector>

#include "contracts/diagnostic.h"
#include "contracts/error_codes.h"
#include "contracts/parameters.h"
#include "edgeflow/log.h"
#include "engine/fixed_batch_executor.h"
#include "engine/text/utf8.h"

namespace llm_edgeflow {

namespace {

struct Params {
  int max_audio_seconds = 30;
  int max_output_bytes = 65536;
};

Parameters<Params> ParamSpec() {
  return Parameters<Params>(
      {Field("max_audio_seconds", &Params::max_audio_seconds)
           .Default(30)
           .Range(1, 60)
           .Description("每段音频的时长上限，单位为秒。"),
       Field("max_output_bytes", &Params::max_output_bytes)
           .Default(65536)
           .Range(1, 65536)
           .Description("每段转写文本的 UTF-8 字节数上限。")});
}

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
    const auto& params = context.Params<Params>();
    auto session = std::dynamic_pointer_cast<IAudioTranscriptionSession>(
        context.backend_session);
    if (!session || session->GetBatchPolicy().max_batch_size != 1 ||
        session->GetBatchPolicy().fixed_batch_size != 0) {
      throw std::runtime_error(
          "whisper_asr requires an IAudioTranscriptionSession with batch "
          "policy {1, 0}");
    }
    auto model = std::make_shared<WhisperAsrModel>();
    model->session_ = std::move(session);
    model->max_audio_seconds_ = params.max_audio_seconds;
    model->max_output_bytes_ = static_cast<size_t>(params.max_output_bytes);
    return model;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(diagnostic, e.what());
    return nullptr;
  } catch (...) {
    SetDiagnosticNoexcept(diagnostic, "Unknown whisper_asr creation error");
    return nullptr;
  }
}

bool WhisperAsrModel::SupportsLanguage(
    std::string_view language) const noexcept {
  return session_ && session_->SupportsLanguage(language);
}

int WhisperAsrModel::Transcribe(const AudioPcmBatch& audio,
                                const TranscribeOptions& options,
                                TextBatch* outputs,
                                std::string* diagnostic) noexcept {
  if (diagnostic) diagnostic->clear();
  if (!outputs) {
    SetDiagnosticNoexcept(diagnostic, "Model output pointer is null");
    return error::kInvalidArgument;
  }
  outputs->clear();
  if (!session_) {
    SetDiagnosticNoexcept(diagnostic, "Model session is null");
    return error::kInvalidState;
  }
  if (audio.empty()) return 0;

  try {
    AudioTranscriptionOptions session_options;
    session_options.language = options.language;
    session_options.max_output_bytes = max_output_bytes_;
    const size_t max_samples = static_cast<size_t>(max_audio_seconds_) * 16000;
    for (const auto& item : audio) {
      if (item.data.sample_rate != 16000) {
        SetDiagnosticNoexcept(diagnostic,
                              "Audio sample rate must be 16000 Hz, received " +
                                  std::to_string(item.data.sample_rate));
        return error::kInvalidInput;
      }
      const size_t n_samples = item.data.pcm_data.size();
      if (n_samples == 0) {
        continue;
      }
      if (n_samples < 1600) {  // 100 毫秒
        SetDiagnosticNoexcept(
            diagnostic,
            "Audio must contain at least 1600 samples (100 ms), received " +
                std::to_string(n_samples));
        return error::kInvalidInput;
      }
      if (n_samples > max_samples) {
        SetDiagnosticNoexcept(
            diagnostic,
            "Audio exceeds model duration limit: " + std::to_string(n_samples) +
                " samples, maximum " + std::to_string(max_samples));
        return error::kInvalidInput;
      }
      for (float s : item.data.pcm_data) {
        if (!std::isfinite(s) || s < -1.0f || s > 1.0f) {
          SetDiagnosticNoexcept(
              diagnostic, "Audio amplitude must be finite and within [-1, 1]");
          return error::kInvalidInput;
        }
      }
    }

    return FixedBatchExecutor::ExecuteItems<AudioPcmPayload, std::string>(
        audio, session_->GetBatchPolicy(),
        [this, &session_options, diagnostic](
            const TraceableItem<AudioPcmPayload>& input, std::string* output) {
          const auto& item = input.data;
          if (item.pcm_data.empty()) {
            output->clear();
            return 0;
          }
          std::string raw_output;
          std::string reason;
          const int ret =
              session_->Transcribe(item, session_options, &raw_output, &reason);
          if (ret != 0) {
            SetDiagnosticNoexcept(diagnostic, reason);
            return ret;
          }
          if (raw_output.find('\0') != std::string::npos) {
            SetDiagnosticNoexcept(diagnostic,
                                  "Transcription contains embedded NUL byte");
            return error::kInvalidOutput;
          }
          std::vector<size_t> boundaries;
          if (!utf8::BuildCodePointBoundaries(raw_output, &boundaries)) {
            SetDiagnosticNoexcept(diagnostic,
                                  "Transcription is not valid UTF-8");
            return error::kInvalidUtf8;
          }
          std::string trimmed = TrimAscii(raw_output);
          if (trimmed.size() > max_output_bytes_) {
            SetDiagnosticNoexcept(diagnostic,
                                  "Transcription exceeds max_output_bytes: " +
                                      std::to_string(trimmed.size()) + " > " +
                                      std::to_string(max_output_bytes_));
            return error::kInvalidOutput;
          }
          *output = std::move(trimmed);
          return 0;
        },
        outputs, diagnostic);
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(diagnostic, e.what());
    outputs->clear();
    return error::kInternalException;
  } catch (...) {
    SetDiagnosticNoexcept(diagnostic, "Unknown transcription exception");
    outputs->clear();
    return error::kUnknownException;
  }
}

static const ModelDefinition& WhisperAsrModelDefinition() {
  static const ModelDefinition definition = [] {
    auto definition = MakeModelDefinition<WhisperAsrModel>();
    definition.description =
        "Whisper automatic speech recognition model for float32 PCM";
    definition.required_protocol = ExecutionProtocol::kAudioTranscription;
    definition.params = ParamSpec();
    return definition;
  }();
  return definition;
}

REGISTER_MODEL_WITH_DEFINITION(WhisperAsrModel, WhisperAsrModelDefinition());

}  // namespace llm_edgeflow

#include "engine/backends/whisper_cpp/whisper_cpp_backend.h"

#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "contracts/diagnostic.h"
#include "contracts/error_codes.h"
#include "contracts/parameters.h"
#include "engine/backend_registry.h"
#include "engine/backends/execution_target.h"

#ifdef HAVE_WHISPERCPP
#include "whisper.h"
#endif

namespace llm_edgeflow {
namespace {

#ifdef HAVE_WHISPERCPP
struct Params {
  int n_threads;
};

Parameters<Params> ParamSpec() {
  return Parameters<Params>({
      Field("n_threads", &Params::n_threads)
          .Default(4)
          .Range(1, 64)
          .Description("CPU 推理线程数"),
  });
}

class WhisperCppSession final : public IAudioTranscriptionSession {
 public:
  WhisperCppSession(whisper_context* ctx, int n_threads)
      : ctx_(ctx), n_threads_(n_threads) {
    is_multilingual_ = (ctx_ && whisper_is_multilingual(ctx_) != 0);
  }

  ~WhisperCppSession() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ctx_) {
      whisper_free(ctx_);
      ctx_ = nullptr;
    }
  }

  const std::string& BackendType() const noexcept override {
    static const std::string type = WhisperCppBackend::kBackendType;
    return type;
  }

  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kAudioTranscription;
  }

  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }

  BatchPolicy GetBatchPolicy() const noexcept override {
    return BatchPolicy{1, 0};
  }

  bool SupportsLanguage(std::string_view language) const noexcept override {
    if (language == "en") return true;
    if (language == "auto") return true;
    if (language == "zh") return is_multilingual_;
    return false;
  }

  /**
   * @brief 对 16 kHz PCM 串行转写，每次调用创建独立的 whisper_state。
   * 在局部缓冲中拼接所有分段文本，容量检查通过后才提交输出。
   * 空音频返回空文本；转写或容量检查失败时保留空输出。
   */
  int Transcribe(const AudioPcmPayload& audio,
                 const AudioTranscriptionOptions& options, std::string* output,
                 std::string* diagnostic = nullptr) noexcept override {
    try {
      if (!output) {
        SetDiagnosticNoexcept(diagnostic, "Output pointer is null");
        return error::kInvalidArgument;
      }
      output->clear();

      if (audio.sample_rate != 16000) {
        SetDiagnosticNoexcept(diagnostic,
                              "whisper_cpp requires 16000 Hz audio");
        return error::kInvalidInput;
      }

      if (audio.pcm_data.empty()) {
        return 0;
      }

      const size_t n_samples = audio.pcm_data.size();
      if (n_samples < 1600) {
        SetDiagnosticNoexcept(
            diagnostic, "Audio duration too short (< 100ms / 1600 samples)");
        return error::kInvalidInput;
      }
      if (n_samples > 960000) {  // 60 秒
        SetDiagnosticNoexcept(diagnostic, "Audio duration exceeds 60s limit");
        return error::kInvalidInput;
      }
      if (n_samples > static_cast<size_t>(std::numeric_limits<int>::max())) {
        SetDiagnosticNoexcept(diagnostic, "Audio duration exceeds int limit");
        return error::kInvalidInput;
      }

      std::lock_guard<std::mutex> lock(mutex_);
      if (!ctx_) {
        SetDiagnosticNoexcept(diagnostic, "whisper context is null");
        return error::kInvalidState;
      }

      std::unique_ptr<whisper_state, decltype(&whisper_free_state)> state(
          whisper_init_state(ctx_), whisper_free_state);
      if (!state) {
        SetDiagnosticNoexcept(diagnostic, "Failed to initialize whisper_state");
        return error::kAllocationFailed;
      }

      auto params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
      params.n_threads = n_threads_;
      std::string lang_str = options.language;
      if (!is_multilingual_ && lang_str == "auto") {
        lang_str = "en";
      }
      params.language = lang_str.c_str();
      params.detect_language = false;
      params.translate = false;
      params.no_context = true;
      params.temperature = 0.0f;
      params.temperature_inc = 0.0f;
      params.greedy.best_of = 1;
      params.no_timestamps = true;
      params.token_timestamps = false;
      params.single_segment = false;
      params.offset_ms = 0;
      params.duration_ms = 0;
      params.print_progress = false;
      params.print_timestamps = false;
      params.print_realtime = false;
      params.print_special = false;
      params.debug_mode = false;
      params.tdrz_enable = false;

      int ret = whisper_full_with_state(ctx_, state.get(), params,
                                        audio.pcm_data.data(),
                                        static_cast<int>(n_samples));
      if (ret != 0) {
        SetDiagnosticNoexcept(
            diagnostic,
            "whisper_full_with_state failed with code: " + std::to_string(ret));
        return error::kInferenceFailed;
      }

      int n_segments = whisper_full_n_segments_from_state(state.get());
      std::string accumulated;
      for (int i = 0; i < n_segments; ++i) {
        const char* seg_text =
            whisper_full_get_segment_text_from_state(state.get(), i);
        if (seg_text) {
          accumulated.append(seg_text);
          if (accumulated.size() > options.max_output_bytes) {
            SetDiagnosticNoexcept(
                diagnostic, "Accumulated output exceeds max_output_bytes: " +
                                std::to_string(options.max_output_bytes));
            output->clear();
            return error::kInvalidOutput;
          }
        }
      }
      *output = std::move(accumulated);
      return 0;
    } catch (const std::exception& e) {
      if (output) output->clear();
      try {
        SetDiagnosticNoexcept(
            diagnostic,
            std::string("Exception during whisper inference: ") + e.what());
      } catch (...) {
        SetDiagnosticNoexcept(diagnostic, "Exception during whisper inference");
      }
      return error::kInternalException;
    } catch (...) {
      if (output) output->clear();
      SetDiagnosticNoexcept(diagnostic,
                            "Unknown exception during whisper inference");
      return error::kUnknownException;
    }
  }

 private:
  std::mutex mutex_;
  whisper_context* ctx_ = nullptr;
  int n_threads_;
  bool is_multilingual_ = false;
};

static const BackendDefinition kWhisperCppBackendDefinition = [] {
  auto def = MakeBackendDefinition<WhisperCppBackend>();
  def.description = "whisper.cpp ASR inference backend";
  def.supported_protocols = {ExecutionProtocol::kAudioTranscription};
  def.concurrency = InferenceConcurrency::kSerialized;
  def.params = ParamSpec();
  return def;
}();

REGISTER_BACKEND_WITH_DEFINITION(WhisperCppBackend,
                                 kWhisperCppBackendDefinition);
#endif

}  // namespace

std::shared_ptr<IBackendSession> WhisperCppBackend::Load(
    const BackendLoadSpec& spec, std::string* diagnostic) noexcept {
  try {
    if (spec.requested_protocol != ExecutionProtocol::kAudioTranscription) {
      SetDiagnosticNoexcept(
          diagnostic,
          "whisper_cpp backend only supports requested protocol "
          "audio_transcription");
      return nullptr;
    }

    const std::string platform =
        NormalizeExecutionPlatform(spec.execution_target.platform);
    if (!platform.empty() && platform != "UNKNOWN" && platform != "CPU") {
      SetDiagnosticNoexcept(
          diagnostic,
          "whisper_cpp backend only supports CPU execution targets, "
          "got platform: " +
              spec.execution_target.platform);
      return nullptr;
    }
    if (spec.execution_target.device_id.has_value() &&
        *spec.execution_target.device_id != 0) {
      SetDiagnosticNoexcept(
          diagnostic,
          "whisper_cpp backend only supports device_id 0 or unset, got: " +
              std::to_string(*spec.execution_target.device_id));
      return nullptr;
    }

    if (spec.model_file.empty()) {
      SetDiagnosticNoexcept(diagnostic, "whisper_cpp model_file is empty");
      return nullptr;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(spec.model_file, ec) || ec) {
      SetDiagnosticNoexcept(
          diagnostic, "whisper_cpp model file not found or not regular file: " +
                          spec.model_file);
      return nullptr;
    }

#ifndef HAVE_WHISPERCPP
    SetDiagnosticNoexcept(diagnostic,
                          "whisper_cpp backend is not enabled in this build");
    return nullptr;
#else
    const auto& p = spec.Params<Params>();
    auto cparams = whisper_context_default_params();
    cparams.use_gpu = false;
    cparams.flash_attn = false;

    std::unique_ptr<whisper_context, decltype(&whisper_free)> context(
        whisper_init_from_file_with_params_no_state(spec.model_file.c_str(),
                                                    cparams),
        whisper_free);
    if (!context) {
      SetDiagnosticNoexcept(
          diagnostic,
          "Failed to load whisper model from file: " + spec.model_file);
      return nullptr;
    }

    auto session =
        std::make_shared<WhisperCppSession>(context.get(), p.n_threads);
    context.release();  // 所有权已成功转移。
    return session;
#endif
  } catch (const std::exception& e) {
    try {
      SetDiagnosticNoexcept(
          diagnostic,
          std::string("Exception loading whisper model: ") + e.what());
    } catch (...) {
      SetDiagnosticNoexcept(diagnostic, "Exception loading whisper model");
    }
    return nullptr;
  } catch (...) {
    SetDiagnosticNoexcept(diagnostic,
                          "Unknown exception loading whisper model");
    return nullptr;
  }
}

}  // namespace llm_edgeflow

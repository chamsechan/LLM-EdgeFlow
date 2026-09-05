#include "engine/backends/whisper_cpp/whisper_cpp_backend.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/backend_registry.h"

#ifdef HAVE_WHISPERCPP
#include "whisper.h"
#endif

namespace llm_edgeflow {
namespace {

void SetDiagnostic(std::string* diagnostic, std::string_view message) noexcept {
  if (!diagnostic) return;
  try {
    *diagnostic = message;
  } catch (...) {
  }
}

std::string NormalizePlatform(std::string_view platform) {
  std::string normalized;
  normalized.reserve(platform.size());
  for (char c : platform) {
    normalized.push_back(
        static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return normalized;
}

#ifdef HAVE_WHISPERCPP
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

  int Transcribe(const AudioPcmPayload& audio,
                 const AudioTranscriptionOptions& options, std::string* output,
                 std::string* diagnostic = nullptr) noexcept override {
    try {
      if (!output) {
        SetDiagnostic(diagnostic, "Output pointer is null");
        return -1;
      }
      output->clear();

      if (audio.sample_rate != 16000) {
        SetDiagnostic(diagnostic, "whisper_cpp requires 16000 Hz audio");
        return -1;
      }

      if (audio.pcm_data.empty()) {
        return 0;
      }

      const size_t n_samples = audio.pcm_data.size();
      if (n_samples < 1600) {
        SetDiagnostic(diagnostic,
                      "Audio duration too short (< 100ms / 1600 samples)");
        return -1;
      }
      if (n_samples > 960000) {  // 60 seconds
        SetDiagnostic(diagnostic, "Audio duration exceeds 60s limit");
        return -1;
      }
      if (n_samples > static_cast<size_t>(std::numeric_limits<int>::max())) {
        SetDiagnostic(diagnostic, "Audio duration exceeds int limit");
        return -1;
      }

      std::lock_guard<std::mutex> lock(mutex_);
      if (!ctx_) {
        SetDiagnostic(diagnostic, "whisper context is null");
        return -1;
      }

      whisper_state* state = whisper_init_state(ctx_);
      if (!state) {
        SetDiagnostic(diagnostic, "Failed to initialize whisper_state");
        return -1;
      }

      struct StateGuard {
        whisper_state* s;
        ~StateGuard() {
          if (s) whisper_free_state(s);
        }
      } state_guard{state};

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

      int ret =
          whisper_full_with_state(ctx_, state, params, audio.pcm_data.data(),
                                  static_cast<int>(n_samples));
      if (ret != 0) {
        SetDiagnostic(diagnostic, "whisper_full_with_state failed with code: " +
                                      std::to_string(ret));
        return -1;
      }

      int n_segments = whisper_full_n_segments_from_state(state);
      std::string accumulated;
      for (int i = 0; i < n_segments; ++i) {
        const char* seg_text =
            whisper_full_get_segment_text_from_state(state, i);
        if (seg_text) {
          accumulated.append(seg_text);
          if (accumulated.size() > options.max_output_bytes) {
            SetDiagnostic(diagnostic,
                          "Accumulated output exceeds max_output_bytes: " +
                              std::to_string(options.max_output_bytes));
            output->clear();
            return -1;
          }
        }
      }
      *output = std::move(accumulated);
      return 0;
    } catch (const std::exception& e) {
      if (output) output->clear();
      try {
        SetDiagnostic(
            diagnostic,
            std::string("Exception during whisper inference: ") + e.what());
      } catch (...) {
        SetDiagnostic(diagnostic, "Exception during whisper inference");
      }
      return -1;
    } catch (...) {
      if (output) output->clear();
      SetDiagnostic(diagnostic, "Unknown exception during whisper inference");
      return -1;
    }
  }

 private:
  std::mutex mutex_;
  whisper_context* ctx_ = nullptr;
  int n_threads_ = 4;
  bool is_multilingual_ = false;
};

static const BackendDefinition kWhisperCppBackendDefinition = [] {
  BackendDefinition def;
  def.backend_type = WhisperCppBackend::kBackendType;
  def.description = "whisper.cpp ASR inference backend";
  def.supported_protocols = {ExecutionProtocol::kAudioTranscription};
  def.concurrency = InferenceConcurrency::kSerialized;
  def.config_fields = {ConfigFieldDefinition{"n_threads",
                                             ConfigValueKind::kInteger,
                                             false,
                                             4,
                                             1.0,
                                             64.0,
                                             {},
                                             "CPU inference thread count"}};
  return def;
}();

REGISTER_BACKEND_WITH_DEFINITION(WhisperCppBackend,
                                 kWhisperCppBackendDefinition);
#endif

}  // namespace

const std::string& WhisperCppBackend::BackendType() const noexcept {
  static const std::string type = kBackendType;
  return type;
}

std::shared_ptr<IBackendSession> WhisperCppBackend::Load(
    const BackendLoadSpec& spec, std::string* diagnostic) noexcept {
  try {
    if (test_load_hook_) {
      test_load_hook_();
    }

    if (spec.requested_protocol.has_value() &&
        *spec.requested_protocol != ExecutionProtocol::kAudioTranscription) {
      SetDiagnostic(diagnostic,
                    "whisper_cpp backend only supports requested protocol "
                    "audio_transcription");
      return nullptr;
    }

    const std::string platform =
        NormalizePlatform(spec.execution_target.platform);
    if (!platform.empty() && platform != "UNKNOWN" && platform != "CPU" &&
        platform != "CPU_GENERIC") {
      SetDiagnostic(diagnostic,
                    "whisper_cpp backend only supports CPU execution targets, "
                    "got platform: " +
                        spec.execution_target.platform);
      return nullptr;
    }
    if (spec.execution_target.device_id.has_value() &&
        *spec.execution_target.device_id != 0) {
      SetDiagnostic(
          diagnostic,
          "whisper_cpp backend only supports device_id 0 or unset, got: " +
              std::to_string(*spec.execution_target.device_id));
      return nullptr;
    }

    if (spec.backend_config.is_object()) {
      for (auto it = spec.backend_config.begin();
           it != spec.backend_config.end(); ++it) {
        if (it.key() != "n_threads") {
          SetDiagnostic(
              diagnostic,
              "Unknown whisper_cpp backend config field: " + it.key());
          return nullptr;
        }
      }
    }

    int n_threads = 4;
    if (spec.backend_config.is_object() &&
        spec.backend_config.contains("n_threads")) {
      const auto& val = spec.backend_config["n_threads"];
      if (!val.is_number_integer()) {
        SetDiagnostic(diagnostic, "n_threads must be an integer");
        return nullptr;
      }
      n_threads = val.get<int>();
      if (n_threads < 1 || n_threads > 64) {
        SetDiagnostic(diagnostic, "n_threads must be between 1 and 64");
        return nullptr;
      }
    }

    if (spec.model_path.empty()) {
      SetDiagnostic(diagnostic, "whisper_cpp model_path is empty");
      return nullptr;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(spec.model_path, ec) || ec) {
      SetDiagnostic(diagnostic,
                    "whisper_cpp model file not found or not regular file: " +
                        spec.model_path);
      return nullptr;
    }

#ifndef HAVE_WHISPERCPP
    SetDiagnostic(diagnostic,
                  "whisper_cpp backend is not enabled in this build");
    return nullptr;
#else
    auto cparams = whisper_context_default_params();
    cparams.use_gpu = false;
    cparams.flash_attn = false;

    whisper_context* raw_ctx = whisper_init_from_file_with_params_no_state(
        spec.model_path.c_str(), cparams);
    if (!raw_ctx) {
      SetDiagnostic(diagnostic, "Failed to load whisper model from file: " +
                                    spec.model_path);
      return nullptr;
    }

    struct ContextGuard {
      whisper_context* ctx = nullptr;
      ~ContextGuard() {
        if (ctx) whisper_free(ctx);
      }
    } context_guard{raw_ctx};

    auto session = std::make_shared<WhisperCppSession>(raw_ctx, n_threads);
    context_guard.ctx = nullptr;  // ownership transferred successfully
    return session;
#endif
  } catch (const std::exception& e) {
    try {
      SetDiagnostic(
          diagnostic,
          std::string("Exception loading whisper model: ") + e.what());
    } catch (...) {
      SetDiagnostic(diagnostic, "Exception loading whisper model");
    }
    return nullptr;
  } catch (...) {
    SetDiagnostic(diagnostic, "Unknown exception loading whisper model");
    return nullptr;
  }
}

}  // namespace llm_edgeflow

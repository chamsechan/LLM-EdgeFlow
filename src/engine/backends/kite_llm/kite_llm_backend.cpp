#include "engine/backends/kite_llm/kite_llm_backend.h"

#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/backend_registry.h"
#include "engine/text/utf8.h"
#include "engine/text_generation/common_autoregressive_generator.h"

#ifdef HAVE_KITELLM
#include "kitellm_edgeflow_adapter.h"
#endif

namespace alg_framework {
namespace {

void SetDiagnostic(std::string* diagnostic,
                   const std::string& message) noexcept {
  if (!diagnostic) return;
  try {
    *diagnostic = message;
  } catch (...) {
  }
}

#ifdef HAVE_KITELLM

bool ResolveRunConfig(const std::string& model_path,
                      const std::string& relative, std::string* resolved,
                      std::string* diagnostic) {
  if (!resolved) return false;
  resolved->clear();
  if (relative.empty()) return true;
  const std::filesystem::path requested(relative);
  if (requested.is_absolute()) {
    SetDiagnostic(diagnostic,
                  "kiteLLM run_config_file must be relative to model_path");
    return false;
  }
  for (const auto& component : requested) {
    if (component == "..") {
      SetDiagnostic(diagnostic,
                    "kiteLLM run_config_file cannot traverse model_path");
      return false;
    }
  }
  std::error_code ec;
  const std::filesystem::path artifact(model_path);
  const auto root = std::filesystem::weakly_canonical(
      std::filesystem::is_directory(artifact, ec) ? artifact
                                                  : artifact.parent_path(),
      ec);
  const auto candidate =
      std::filesystem::weakly_canonical(root / requested, ec);
  if (ec || !std::filesystem::is_regular_file(candidate, ec) || ec) {
    SetDiagnostic(diagnostic,
                  "kiteLLM run-config file does not exist: " + relative);
    return false;
  }
  auto root_it = root.begin();
  auto candidate_it = candidate.begin();
  for (; root_it != root.end(); ++root_it, ++candidate_it) {
    if (candidate_it == candidate.end() || *root_it != *candidate_it) {
      SetDiagnostic(diagnostic, "kiteLLM run_config_file escapes model_path");
      return false;
    }
  }
  *resolved = candidate.string();
  return true;
}

struct KiteHandleDeleter {
  void operator()(kitellm_edgeflow_handle* handle) const noexcept {
    if (handle) kitellm_edgeflow_destroy(handle);
  }
};

using KiteHandlePtr =
    std::unique_ptr<kitellm_edgeflow_handle, KiteHandleDeleter>;

class KiteResultGuard final {
 public:
  explicit KiteResultGuard(kitellm_edgeflow_result* result) : result_(result) {}
  ~KiteResultGuard() {
    if (result_) kitellm_edgeflow_result_release(result_);
  }
  KiteResultGuard(const KiteResultGuard&) = delete;
  KiteResultGuard& operator=(const KiteResultGuard&) = delete;

 private:
  kitellm_edgeflow_result* result_ = nullptr;
};

class KiteTextGenerationSession final : public ITextGenerationSession {
 public:
  explicit KiteTextGenerationSession(KiteHandlePtr handle)
      : handle_(std::move(handle)) {}

  const std::string& BackendType() const noexcept override {
    static const std::string type = KiteLlmBackend::kBackendType;
    return type;
  }
  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kTextGeneration;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
  BatchPolicy GetBatchPolicy() const noexcept override {
    return BatchPolicy{1, 0};
  }

  int Generate(const std::string& formatted_prompt, bool add_bos,
               const GenerateOptions& options, std::optional<uint64_t> seed,
               std::string* output, std::string* diagnostic) noexcept override {
    if (!output) {
      SetDiagnostic(diagnostic, "kiteLLM output pointer is null");
      return -1;
    }
    output->clear();
    if (!handle_) {
      SetDiagnostic(diagnostic, "kiteLLM handle is not initialized");
      return -1;
    }
    if (seed.has_value()) {
      SetDiagnostic(diagnostic,
                    "kiteLLM adapter does not support a fixed random seed");
      return -1;
    }
    if (formatted_prompt.empty()) {
      SetDiagnostic(diagnostic, "Formatted prompt is empty");
      return -1;
    }
    if (!text_generation::ValidateGenerateOptions(options, diagnostic)) {
      return -1;
    }

    try {
      std::vector<const char*> stop_words;
      std::vector<size_t> stop_word_sizes;
      stop_words.reserve(options.stop_words.size());
      stop_word_sizes.reserve(options.stop_words.size());
      for (const auto& word : options.stop_words) {
        stop_words.push_back(word.data());
        stop_word_sizes.push_back(word.size());
      }

      kitellm_edgeflow_generate_options vendor_options{};
      vendor_options.struct_size = sizeof(vendor_options);
      vendor_options.max_tokens = options.max_tokens;
      vendor_options.temperature = options.temperature;
      vendor_options.top_k = options.top_k;
      vendor_options.top_p = options.top_p;
      vendor_options.repetition_penalty = options.repetition_penalty;
      vendor_options.stop_words = stop_words.data();
      vendor_options.stop_word_sizes = stop_word_sizes.data();
      vendor_options.stop_word_count = stop_words.size();

      kitellm_edgeflow_result vendor_result{};
      vendor_result.struct_size = sizeof(vendor_result);
      std::lock_guard<std::mutex> lock(generate_mutex_);
      const int result = kitellm_edgeflow_generate(
          handle_.get(), formatted_prompt.data(), formatted_prompt.size(),
          add_bos ? 1 : 0, &vendor_options, &vendor_result);
      KiteResultGuard result_guard(&vendor_result);
      if (result != 0 || (!vendor_result.data && vendor_result.size > 0)) {
        const char* error = kitellm_edgeflow_last_error(handle_.get());
        SetDiagnostic(diagnostic,
                      std::string("kiteLLM generation failed") +
                          (error && *error ? ": " + std::string(error) : ""));
        return -1;
      }
      output->assign(vendor_result.data ? vendor_result.data : "",
                     vendor_result.size);
      utf8::StripIncompleteSuffix(output);
      return 0;
    } catch (const std::exception& e) {
      output->clear();
      SetDiagnostic(diagnostic,
                    std::string("kiteLLM generation exception: ") + e.what());
      return -1;
    } catch (...) {
      output->clear();
      SetDiagnostic(diagnostic, "Unknown kiteLLM generation exception");
      return -1;
    }
  }

 private:
  KiteHandlePtr handle_;
  std::mutex generate_mutex_;
};

#endif  // HAVE_KITELLM

}  // namespace

const std::string& KiteLlmBackend::BackendType() const noexcept {
  static const std::string type = kBackendType;
  return type;
}

std::shared_ptr<IBackendSession> KiteLlmBackend::Load(
    const BackendLoadSpec& spec, std::string* diagnostic) noexcept {
  if (spec.requested_protocol.has_value() &&
      *spec.requested_protocol != ExecutionProtocol::kTextGeneration) {
    SetDiagnostic(
        diagnostic,
        "kiteLLM does not support requested protocol: " +
            std::string(ExecutionProtocolName(*spec.requested_protocol)));
    return nullptr;
  }
  if (spec.execution_target.device_id.has_value() ||
      (!spec.execution_target.platform.empty() &&
       spec.execution_target.platform != "UNKNOWN")) {
    SetDiagnostic(diagnostic,
                  "kiteLLM execution target must be configured in "
                  "run_config_file");
    return nullptr;
  }
#ifndef HAVE_KITELLM
  (void)spec;
  SetDiagnostic(diagnostic, "kiteLLM SDK was not compiled into this build");
  return nullptr;
#else
  try {
    if (spec.model_path.empty()) {
      SetDiagnostic(diagnostic, "kiteLLM model path is empty");
      return nullptr;
    }
    std::error_code ec;
    if (!std::filesystem::exists(spec.model_path, ec) || ec) {
      SetDiagnostic(diagnostic,
                    "kiteLLM model path does not exist: " + spec.model_path);
      return nullptr;
    }
    if (!spec.backend_config.is_object()) {
      SetDiagnostic(diagnostic, "kiteLLM backend_config must be an object");
      return nullptr;
    }
    for (const auto& [key, value] : spec.backend_config.items()) {
      (void)value;
      if (key != "run_config_file") {
        SetDiagnostic(diagnostic,
                      "Unknown kiteLLM backend_config field: " + key);
        return nullptr;
      }
    }
    const std::string run_config_file =
        spec.backend_config.value("run_config_file", "");
    std::string resolved_run_config;
    if (!ResolveRunConfig(spec.model_path, run_config_file,
                          &resolved_run_config, diagnostic)) {
      return nullptr;
    }

    kitellm_edgeflow_handle* raw_handle = nullptr;
    const int result = kitellm_edgeflow_create(
        spec.model_path.c_str(), resolved_run_config.c_str(), &raw_handle);
    KiteHandlePtr handle(raw_handle);
    if (result != 0 || !handle) {
      const char* error =
          raw_handle ? kitellm_edgeflow_last_error(raw_handle) : nullptr;
      SetDiagnostic(diagnostic,
                    std::string("kiteLLM create failed") +
                        (error && *error ? ": " + std::string(error) : ""));
      return nullptr;
    }
    return std::make_shared<KiteTextGenerationSession>(std::move(handle));
  } catch (const std::exception& e) {
    SetDiagnostic(diagnostic,
                  std::string("kiteLLM load exception: ") + e.what());
    return nullptr;
  } catch (...) {
    SetDiagnostic(diagnostic, "Unknown kiteLLM load exception");
    return nullptr;
  }
#endif
}

#ifdef HAVE_KITELLM
static const BackendDefinition kKiteLlmBackendDefinition = [] {
  BackendDefinition definition;
  definition.backend_type = KiteLlmBackend::kBackendType;
  definition.description = "kiteLLM managed text-generation backend";
  definition.supported_protocols = {ExecutionProtocol::kTextGeneration};
  definition.concurrency = InferenceConcurrency::kSerialized;
  definition.config_fields = {
      {"run_config_file",
       ConfigValueKind::kString,
       false,
       "",
       std::nullopt,
       std::nullopt,
       {},
       "Optional model-relative kiteLLM run-config file; also owns device "
       "selection"}};
  return definition;
}();

REGISTER_BACKEND_WITH_DEFINITION(KiteLlmBackend, kKiteLlmBackendDefinition);
#endif

}  // namespace alg_framework

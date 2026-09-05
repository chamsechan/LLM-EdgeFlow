#include "engine/backends/kite_llm/kite_llm_backend.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "engine/backend_registry.h"
#include "engine/text/utf8.h"
#include "engine/text_generation/common_autoregressive_generator.h"

#ifdef HAVE_KITELLM
#include <kiteLLM.h>
#endif

namespace llm_edgeflow {
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

// Runtime must outlive every model handle, including failed session
// construction.
class KiteRuntime final {
 public:
  KiteRuntime() { kiteLLM_Init(); }
  ~KiteRuntime() { kiteLLM_DeInit(); }
  KiteRuntime(const KiteRuntime&) = delete;
  KiteRuntime& operator=(const KiteRuntime&) = delete;
};

template <void (*Release)(void*)>
struct KiteDeleter {
  void operator()(void* value) const noexcept {
    if (value) Release(value);
  }
};

using KiteHandlePtr = std::unique_ptr<void, KiteDeleter<kiteLLM_Unload>>;
using KiteParameterPtr =
    std::unique_ptr<void, KiteDeleter<kiteLLM_Parameter_Deallocate>>;
using KiteInputPtr =
    std::unique_ptr<void, KiteDeleter<kiteLLM_TaskInput_Deallocate>>;
using KiteOutputPtr =
    std::unique_ptr<void, KiteDeleter<kiteLLM_TaskOutput_Deallocate>>;

void CheckKiteStatus(int status, const char* operation) {
  if (status != KLLM_OK) {
    throw std::runtime_error(std::string(operation) +
                             " failed with kiteLLM status " +
                             std::to_string(status));
  }
}

class KiteTextGenerationSession final : public ITextGenerationSession {
 public:
  KiteTextGenerationSession(std::shared_ptr<KiteRuntime> runtime,
                            KiteHandlePtr handle)
      : runtime_(std::move(runtime)), handle_(std::move(handle)) {}

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
                    "kiteLLM does not support a fixed random seed per request");
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
      if (formatted_prompt.size() >
          static_cast<size_t>(std::numeric_limits<int>::max())) {
        SetDiagnostic(diagnostic,
                      "kiteLLM prompt exceeds tokenizer length limit");
        return -1;
      }
      std::lock_guard<std::mutex> lock(generate_mutex_);
      // The Model already formatted the prompt. Token input preserves special
      // tokens and the explicit BOS policy without a second chat template.
      int token_count = 0;
      const int probe = kiteLLM_Tokenizer_Encode(
          handle_.get(), formatted_prompt.data(),
          static_cast<int>(formatted_prompt.size()), nullptr, 0, &token_count,
          add_bos ? 1 : 0, 1);
      if (probe != KLLM_E_BUFFER_TOO_SMALL) CheckKiteStatus(probe, "Tokenize");
      if (token_count <= 0) {
        SetDiagnostic(diagnostic, "kiteLLM prompt produced no tokens");
        return -1;
      }
      std::vector<int> tokens(static_cast<size_t>(token_count));
      CheckKiteStatus(
          kiteLLM_Tokenizer_Encode(handle_.get(), formatted_prompt.data(),
                                   static_cast<int>(formatted_prompt.size()),
                                   tokens.data(), token_count, &token_count,
                                   add_bos ? 1 : 0, 1),
          "Tokenize");
      KiteInputPtr input(kiteLLM_TaskInput_Allocate());
      if (!input) throw std::runtime_error("kiteLLM task allocation failed");
      CheckKiteStatus(kiteLLM_TaskInput_SetPromptTokens(
                          input.get(), tokens.data(), token_count),
                      "SetPromptTokens");
      CheckKiteStatus(
          kiteLLM_TaskInput_SetMaxOutputTokens(input.get(), options.max_tokens),
          "SetMaxOutputTokens");
      CheckKiteStatus(
          kiteLLM_TaskInput_SetTemperature(input.get(), options.temperature),
          "SetTemperature");
      CheckKiteStatus(kiteLLM_TaskInput_SetTopK(input.get(), options.top_k),
                      "SetTopK");
      CheckKiteStatus(kiteLLM_TaskInput_SetTopP(input.get(), options.top_p),
                      "SetTopP");
      CheckKiteStatus(kiteLLM_TaskInput_SetRepetitionPenalty(
                          input.get(), options.repetition_penalty),
                      "SetRepetitionPenalty");
      void* raw_output = nullptr;
      const int status = kiteLLM_Run(handle_.get(), input.get(), &raw_output);
      KiteOutputPtr result(raw_output);
      CheckKiteStatus(status, "Run");
      if (!result)
        throw std::runtime_error("kiteLLM returned a null task output");
      int output_token_count = 0;
      const int* output_tokens =
          kiteLLM_TaskOutput_GetResultTokens(result.get(), &output_token_count);
      if (output_token_count < 0 ||
          (output_token_count > 0 && !output_tokens)) {
        throw std::runtime_error("kiteLLM returned invalid output tokens");
      }
      if (output_token_count > 0) {
        int text_size = 0;
        const int decode_probe = kiteLLM_Tokenizer_Decode(
            handle_.get(), output_tokens, output_token_count, nullptr, 0,
            &text_size, 0);
        if (decode_probe != KLLM_E_BUFFER_TOO_SMALL)
          CheckKiteStatus(decode_probe, "Decode");
        if (text_size < 0 || text_size == std::numeric_limits<int>::max()) {
          throw std::runtime_error("kiteLLM returned invalid output length");
        }
        std::string decoded(static_cast<size_t>(text_size) + 1, '\0');
        CheckKiteStatus(kiteLLM_Tokenizer_Decode(
                            handle_.get(), output_tokens, output_token_count,
                            decoded.data(), static_cast<int>(decoded.size()),
                            &text_size, 0),
                        "Decode");
        decoded.resize(static_cast<size_t>(text_size));
        size_t stop_position = decoded.size();
        for (const auto& word : options.stop_words) {
          stop_position = std::min(stop_position, decoded.find(word));
        }
        decoded.resize(stop_position);
        *output = std::move(decoded);
      }
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
  std::shared_ptr<KiteRuntime> runtime_;
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
    if (!std::filesystem::is_regular_file(spec.model_path, ec) || ec) {
      SetDiagnostic(diagnostic, "kiteLLM model path is not a regular file: " +
                                    spec.model_path);
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

    auto runtime = std::make_shared<KiteRuntime>();
    KiteParameterPtr parameters(kiteLLM_Parameter_Allocate());
    if (!parameters) {
      SetDiagnostic(diagnostic, "kiteLLM parameter allocation failed");
      return nullptr;
    }
    kiteLLM_Parameter_SetLoadFromFileSync(parameters.get(), 1);
    if (!resolved_run_config.empty()) {
      kiteLLM_Parameter_SetRunConfigFile(parameters.get(),
                                         resolved_run_config.c_str());
    }
    KiteHandlePtr handle(
        kiteLLM_LoadFromFile(spec.model_path.c_str(), parameters.get()));
    if (!handle) {
      SetDiagnostic(diagnostic,
                    "kiteLLM model load failed: " + spec.model_path);
      return nullptr;
    }
    return std::make_shared<KiteTextGenerationSession>(std::move(runtime),
                                                       std::move(handle));
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

}  // namespace llm_edgeflow

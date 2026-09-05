#include "engine/backends/kite_llm/kite_llm_backend.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
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

bool IsCpuPlatform(const std::string& platform) {
  return platform == "CPU" || platform == "CPU_GENERIC";
}

bool ValidateExecutionTarget(const ExecutionTarget& target,
                             std::string* diagnostic) {
  std::string platform = target.platform;
  std::transform(platform.begin(), platform.end(), platform.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  if (!platform.empty() && platform != "UNKNOWN" && !IsCpuPlatform(platform)) {
    SetDiagnostic(diagnostic,
                  "Pinned kiteLLM release supports CPU execution only; "
                  "requested platform: " +
                      target.platform);
    return false;
  }
  if (target.device_id.has_value()) {
    const int device = *target.device_id;
    if (device < -1) {
      SetDiagnostic(diagnostic, "kiteLLM device_id must be >= -1");
      return false;
    }
    if (IsCpuPlatform(platform) && device > 0) {
      SetDiagnostic(diagnostic,
                    "kiteLLM CPU execution only accepts device_id 0 or "
                    "automatic selection (-1)");
      return false;
    }
  }
  return true;
}

#ifdef HAVE_KITELLM

bool ValidateCpuRunConfig(const std::string& platform,
                          const nlohmann::json& config,
                          std::string* diagnostic) {
  std::string normalized = platform;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  if (!IsCpuPlatform(normalized)) return true;
  // Only enforce the explicit CPU constraint; the SDK owns its full schema.
  if (config.is_object() && config.contains("model") &&
      config["model"].is_object()) {
    const auto& model = config["model"];
    if (model.contains("gpu_layers") && model["gpu_layers"].is_number() &&
        model["gpu_layers"].get<double>() > 0) {
      SetDiagnostic(diagnostic,
                    "kiteLLM CPU execution conflicts with run-config "
                    "model.gpu_layers > 0");
      return false;
    }
  }
  return true;
}

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

template <typename Interface, ExecutionProtocol protocol>
class KiteSession : public Interface {
 public:
  KiteSession(std::shared_ptr<KiteRuntime> runtime, KiteHandlePtr handle)
      : runtime_(std::move(runtime)), handle_(std::move(handle)) {}

  const std::string& BackendType() const noexcept override {
    static const std::string type = KiteLlmBackend::kBackendType;
    return type;
  }
  ExecutionProtocol Protocol() const noexcept override { return protocol; }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
  BatchPolicy GetBatchPolicy() const noexcept override {
    return BatchPolicy{1, 0};
  }

 protected:
  KiteInputPtr PreparePrompt(const std::string& prompt, bool add_bos) {
    if (prompt.empty() ||
        prompt.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
      throw std::runtime_error("Invalid kiteLLM prompt length");
    }
    // The Model owns formatting. Preserve special tokens and the BOS policy
    // using token input, avoiding a second vendor chat template.
    int count = 0;
    const int probe = kiteLLM_Tokenizer_Encode(
        handle_.get(), prompt.data(), static_cast<int>(prompt.size()), nullptr,
        0, &count, add_bos ? 1 : 0, 1);
    if (probe != KLLM_E_BUFFER_TOO_SMALL) CheckKiteStatus(probe, "Tokenize");
    if (count <= 0)
      throw std::runtime_error("kiteLLM prompt produced no tokens");
    const int capacity = count;
    std::vector<int> tokens(static_cast<size_t>(capacity));
    CheckKiteStatus(
        kiteLLM_Tokenizer_Encode(handle_.get(), prompt.data(),
                                 static_cast<int>(prompt.size()), tokens.data(),
                                 capacity, &count, add_bos ? 1 : 0, 1),
        "Tokenize");
    if (count <= 0 || count > capacity)
      throw std::runtime_error("kiteLLM returned invalid input token count");
    KiteInputPtr input(kiteLLM_TaskInput_Allocate());
    if (!input) throw std::runtime_error("kiteLLM task allocation failed");
    CheckKiteStatus(
        kiteLLM_TaskInput_SetPromptTokens(input.get(), tokens.data(), count),
        "SetPromptTokens");
    return input;
  }

  KiteOutputPtr ExecuteTask(void* input, const GenerateOptions& options) {
    CheckKiteStatus(
        kiteLLM_TaskInput_SetMaxOutputTokens(input, options.max_tokens),
        "SetMaxOutputTokens");
    CheckKiteStatus(
        kiteLLM_TaskInput_SetTemperature(input, options.temperature),
        "SetTemperature");
    CheckKiteStatus(kiteLLM_TaskInput_SetTopK(input, options.top_k), "SetTopK");
    CheckKiteStatus(kiteLLM_TaskInput_SetTopP(input, options.top_p), "SetTopP");
    CheckKiteStatus(kiteLLM_TaskInput_SetRepetitionPenalty(
                        input, options.repetition_penalty),
                    "SetRepetitionPenalty");
    void* raw_output = nullptr;
    const int status = kiteLLM_Run(handle_.get(), input, &raw_output);
    KiteOutputPtr result(raw_output);
    CheckKiteStatus(status, "Run");
    if (!result)
      throw std::runtime_error("kiteLLM returned a null task output");
    return result;
  }

  void RunTask(void* input, const GenerateOptions& options,
               std::string* output) {
    auto result = ExecuteTask(input, options);
    int output_token_count = 0;
    const int* output_tokens =
        kiteLLM_TaskOutput_GetResultTokens(result.get(), &output_token_count);
    if (output_token_count < 0 || (output_token_count > 0 && !output_tokens)) {
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
      CheckKiteStatus(
          kiteLLM_Tokenizer_Decode(
              handle_.get(), output_tokens, output_token_count, decoded.data(),
              static_cast<int>(decoded.size()), &text_size, 0),
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
  }

  std::shared_ptr<KiteRuntime> runtime_;
  KiteHandlePtr handle_;
  std::mutex session_mutex_;
};

using KiteTextSessionBase =
    KiteSession<ITextGenerationSession, ExecutionProtocol::kTextGeneration>;
class KiteTextGenerationSession final : public KiteTextSessionBase {
 public:
  using KiteTextSessionBase::KiteTextSessionBase;
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
      std::lock_guard<std::mutex> lock(session_mutex_);
      auto input = PreparePrompt(formatted_prompt, add_bos);
      RunTask(input.get(), options, output);
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
};

using KiteEmbeddingSessionBase =
    KiteSession<IGeneratedTokenEmbeddingSession,
                ExecutionProtocol::kGeneratedTokenEmbedding>;
class KiteGeneratedTokenEmbeddingSession final
    : public KiteEmbeddingSessionBase {
 public:
  using KiteEmbeddingSessionBase::KiteEmbeddingSessionBase;

  int GenerateEmbeddings(const std::string& prompt, bool add_bos,
                         int max_tokens, GeneratedTokenEmbeddings* output,
                         std::string* diagnostic) noexcept override {
    if (!output) {
      SetDiagnostic(diagnostic, "kiteLLM embedding output pointer is null");
      return -1;
    }
    *output = {};
    try {
      if (max_tokens < 1 || max_tokens > 64)
        throw std::runtime_error("Embedding max_tokens must be in [1, 64]");
      std::lock_guard<std::mutex> lock(session_mutex_);
      auto input = PreparePrompt(prompt, add_bos);
      CheckKiteStatus(kiteLLM_TaskInput_SetVerboseDataFlag(
                          input.get(), KLLM_VERBOSE_F_OUTPUT_EMBEDDING),
                      "SetVerboseDataFlag");
      CheckKiteStatus(kiteLLM_TaskInput_SetBeamSize(input.get(), 1),
                      "SetBeamSize");
      GenerateOptions options;
      options.max_tokens = max_tokens;
      options.temperature = 0.0f;
      options.top_p = 1.0f;
      auto result = ExecuteTask(input.get(), options);
      int token_count = 0, rows = 0, dim = 0;
      const int* tokens =
          kiteLLM_TaskOutput_GetResultTokens(result.get(), &token_count);
      const float** values =
          kiteLLM_TaskOutput_GetOutputEmbedding(result.get(), &rows, &dim);
      if (token_count < 0 || token_count > max_tokens || rows != token_count ||
          dim < 0 || dim > 65536 ||
          (rows > 0 && (!tokens || !values || dim == 0))) {
        throw std::runtime_error(
            "kiteLLM returned missing or invalid token embeddings");
      }
      GeneratedTokenEmbeddings staged;
      for (int r = 0; r < rows; ++r) {
        if (!values[r] || tokens[r] < 0)
          throw std::runtime_error(
              "kiteLLM returned an invalid embedding row or token");
        std::vector<float> row(values[r], values[r] + dim);
        if (!std::all_of(row.begin(), row.end(),
                         [](float v) { return std::isfinite(v); }))
          throw std::runtime_error("kiteLLM returned non-finite embeddings");
        staged.token_ids.push_back(tokens[r]);
        staged.values.push_back(std::move(row));
      }
      *output = std::move(staged);
      return 0;
    } catch (const std::exception& e) {
      SetDiagnostic(diagnostic, e.what());
      return -1;
    } catch (...) {
      SetDiagnostic(diagnostic, "Unknown kiteLLM embedding exception");
      return -1;
    }
  }
};

using KiteImageSessionBase =
    KiteSession<IImageTextGenerationSession,
                ExecutionProtocol::kImageTextGeneration>;
class KiteImageTextGenerationSession final : public KiteImageSessionBase {
 public:
  using KiteImageSessionBase::KiteImageSessionBase;

  int Generate(const ImageTextInput& request, const GenerateOptions& options,
               std::string* output, std::string* diagnostic) noexcept override {
    if (!output) {
      SetDiagnostic(diagnostic, "kiteLLM output pointer is null");
      return -1;
    }
    output->clear();
    try {
      if (request.prompt.empty() ||
          request.prompt.find('\0') != std::string::npos ||
          request.width <= 0 || request.height <= 0 ||
          request.patch_size <= 0 || request.width % request.patch_size != 0 ||
          request.height % request.patch_size != 0 ||
          request.rgb_chw.size() > 64U * 1024U * 1024U ||
          static_cast<uint64_t>(request.width) * request.height * 3 !=
              request.rgb_chw.size()) {
        SetDiagnostic(
            diagnostic,
            "Invalid image-text input dimensions, RGB planes or prompt");
        return -1;
      }
      if (!text_generation::ValidateGenerateOptions(options, diagnostic))
        return -1;
      std::lock_guard<std::mutex> lock(session_mutex_);
      KiteInputPtr input(kiteLLM_TaskInput_Allocate());
      if (!input) throw std::runtime_error("kiteLLM task allocation failed");
      kiteLLM_MultiModal_ChatContent content[2]{};
      content[0].type = "image";
      content[0].data = request.rgb_chw.data();
      content[0].size = static_cast<int>(request.rgb_chw.size());
      content[0].data_type = 4;
      content[0].grid_t = 1;
      content[0].grid_h = request.height / request.patch_size;
      content[0].grid_w = request.width / request.patch_size;
      content[1].type = "text";
      content[1].data = request.prompt.c_str();
      kiteLLM_MultiModal_ChatHistory history{};
      history.role = "user";
      history.content = content;
      history.length = 2;
      CheckKiteStatus(
          kiteLLM_TaskInput_SetMultiModal_ChatHistory(input.get(), &history, 1),
          "SetMultiModalChatHistory");
      RunTask(input.get(), options, output);
      return 0;
    } catch (const std::exception& e) {
      output->clear();
      SetDiagnostic(
          diagnostic,
          std::string("kiteLLM image generation exception: ") + e.what());
      return -1;
    } catch (...) {
      output->clear();
      SetDiagnostic(diagnostic, "Unknown kiteLLM image generation exception");
      return -1;
    }
  }
};

#endif  // HAVE_KITELLM

}  // namespace

const std::string& KiteLlmBackend::BackendType() const noexcept {
  static const std::string type = kBackendType;
  return type;
}

std::shared_ptr<IBackendSession> KiteLlmBackend::Load(
    const BackendLoadSpec& spec, std::string* diagnostic) noexcept {
  try {
    if (spec.requested_protocol.has_value() &&
        *spec.requested_protocol != ExecutionProtocol::kTextGeneration &&
        *spec.requested_protocol != ExecutionProtocol::kImageTextGeneration &&
        *spec.requested_protocol !=
            ExecutionProtocol::kGeneratedTokenEmbedding) {
      SetDiagnostic(
          diagnostic,
          "kiteLLM does not support requested protocol: " +
              std::string(ExecutionProtocolName(*spec.requested_protocol)));
      return nullptr;
    }
    if (!ValidateExecutionTarget(spec.execution_target, diagnostic)) {
      return nullptr;
    }
#ifndef HAVE_KITELLM
    SetDiagnostic(diagnostic, "kiteLLM SDK was not compiled into this build");
    return nullptr;
#else
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

    const bool image_protocol =
        spec.requested_protocol == ExecutionProtocol::kImageTextGeneration;
    nlohmann::json run_config;
    if (!resolved_run_config.empty()) {
      std::ifstream input(resolved_run_config);
      if (!input)
        throw std::runtime_error("Cannot read kiteLLM run-config file");
      run_config = nlohmann::json::parse(input, nullptr, false);
    }
    if (!ValidateCpuRunConfig(spec.execution_target.platform, run_config,
                              diagnostic)) {
      return nullptr;
    }
    if (image_protocol &&
        (!run_config.is_object() || !run_config.contains("vision"))) {
      SetDiagnostic(diagnostic,
                    "image_text_generation requires run-config vision.mmproj");
      return nullptr;
    }
    if (run_config.is_object() && run_config.contains("vision")) {
      const auto& vision = run_config["vision"];
      if (!vision.is_object() || !vision.contains("mmproj") ||
          !vision["mmproj"].is_string() ||
          vision["mmproj"].get<std::string>().empty()) {
        SetDiagnostic(diagnostic, "Invalid run-config vision.mmproj");
        return nullptr;
      }
      std::string projector;
      if (!ResolveRunConfig(resolved_run_config,
                            vision["mmproj"].get<std::string>(), &projector,
                            diagnostic))
        return nullptr;
    }

    auto runtime = std::make_shared<KiteRuntime>();
    KiteParameterPtr parameters(kiteLLM_Parameter_Allocate());
    if (!parameters) {
      SetDiagnostic(diagnostic, "kiteLLM parameter allocation failed");
      return nullptr;
    }
    kiteLLM_Parameter_SetLoadFromFileSync(parameters.get(), 1);
    if (spec.execution_target.device_id.has_value()) {
      kiteLLM_Parameter_SetDeviceId(parameters.get(),
                                    *spec.execution_target.device_id);
    }
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
    if (spec.requested_protocol ==
        ExecutionProtocol::kGeneratedTokenEmbedding) {
      return std::make_shared<KiteGeneratedTokenEmbeddingSession>(
          std::move(runtime), std::move(handle));
    }
    if (image_protocol) {
      return std::make_shared<KiteImageTextGenerationSession>(
          std::move(runtime), std::move(handle));
    }
    return std::make_shared<KiteTextGenerationSession>(std::move(runtime),
                                                       std::move(handle));
#endif
  } catch (const std::exception& e) {
    SetDiagnostic(diagnostic,
                  std::string("kiteLLM load exception: ") + e.what());
    return nullptr;
  } catch (...) {
    SetDiagnostic(diagnostic, "Unknown kiteLLM load exception");
    return nullptr;
  }
}

#ifdef HAVE_KITELLM
static const BackendDefinition kKiteLlmBackendDefinition = [] {
  BackendDefinition definition;
  definition.backend_type = KiteLlmBackend::kBackendType;
  definition.description =
      "kiteLLM text/image generation and generated-token embeddings (pinned "
      "CPU release; native "
      "device ID)";
  definition.supported_protocols = {
      ExecutionProtocol::kTextGeneration,
      ExecutionProtocol::kImageTextGeneration,
      ExecutionProtocol::kGeneratedTokenEmbedding};
  definition.concurrency = InferenceConcurrency::kSerialized;
  definition.config_fields = {
      {"run_config_file",
       ConfigValueKind::kString,
       false,
       "",
       std::nullopt,
       std::nullopt,
       {},
       "Optional model-relative kiteLLM run-config file; device ID is passed "
       "through the native parameter API"}};
  return definition;
}();

REGISTER_BACKEND_WITH_DEFINITION(KiteLlmBackend, kKiteLlmBackendDefinition);
#endif

}  // namespace llm_edgeflow

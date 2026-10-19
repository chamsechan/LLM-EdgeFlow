#include "engine/backends/inferlet/inferlet_backend.h"

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

#include "contracts/diagnostic.h"
#include "contracts/error_codes.h"
#include "contracts/parameters.h"
#include "contracts/path_utils.h"
#include "engine/backend_registry.h"
#include "engine/text/utf8.h"
#include "engine/text_generation/common_autoregressive_generator.h"

#ifdef HAVE_INFERLET
#include <inferlet.h>
#endif

namespace llm_edgeflow {

namespace {

bool IsCpuPlatform(const std::string& platform) { return platform == "CPU"; }

bool ValidateExecutionTarget(const ExecutionTarget& target,
                             std::string* diagnostic) {
  std::string platform = target.platform;
  std::transform(platform.begin(), platform.end(), platform.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  if (!platform.empty() && platform != "UNKNOWN" && !IsCpuPlatform(platform)) {
    SetDiagnosticNoexcept(
        diagnostic,
        "Pinned Inferlet release supports CPU execution only; "
        "requested platform: " +
            target.platform);
    return false;
  }
  if (target.device_id.has_value()) {
    const int device = *target.device_id;
    if (device < -1) {
      SetDiagnosticNoexcept(diagnostic, "Inferlet device_id must be >= -1");
      return false;
    }
    if (device > 0) {
      SetDiagnosticNoexcept(
          diagnostic,
          "Inferlet CPU execution only accepts device_id 0 or "
          "automatic selection (-1)");
      return false;
    }
  }
  return true;
}

#ifdef HAVE_INFERLET

struct Params {
  std::optional<std::string> run_config_file;
};

Parameters<Params> ParamSpec() {
  return Parameters<Params>({
      Field("run_config_file", &Params::run_config_file)
          .File()
          .Description("可选的 Inferlet 运行配置文件，相对 Pipeline "
                       "文件目录解析；不写表示"
                       "不用运行配置；设备 ID "
                       "通过原生参数接口传入"),
  });
}

bool ValidateCpuRunConfig(const nlohmann::json& config,
                          std::string* diagnostic) {
  // 只强制显式的 CPU 约束；完整 schema 归 SDK 所有。
  if (config.is_object() && config.contains("model") &&
      config["model"].is_object()) {
    const auto& model = config["model"];
    if (model.contains("gpu_layers") && model["gpu_layers"].is_number() &&
        model["gpu_layers"].get<double>() > 0) {
      SetDiagnosticNoexcept(diagnostic,
                            "Inferlet CPU execution conflicts with run-config "
                            "model.gpu_layers > 0");
      return false;
    }
  }
  return true;
}

// Runtime 的生命周期必须长于所有模型句柄，包括会话构造失败的情况。
class InferletRuntime final {
 public:
  InferletRuntime() { inferlet_init(); }
  ~InferletRuntime() { inferlet_deinit(); }
  InferletRuntime(const InferletRuntime&) = delete;
  InferletRuntime& operator=(const InferletRuntime&) = delete;
};

template <void (*Release)(void*)>
struct InferletDeleter {
  void operator()(void* value) const noexcept {
    if (value) Release(value);
  }
};

using InferletHandlePtr =
    std::unique_ptr<void, InferletDeleter<inferlet_unload>>;
using InferletParameterPtr =
    std::unique_ptr<void, InferletDeleter<inferlet_parameter_deallocate>>;
using InferletInputPtr =
    std::unique_ptr<void, InferletDeleter<inferlet_task_input_deallocate>>;
using InferletOutputPtr =
    std::unique_ptr<void, InferletDeleter<inferlet_task_output_deallocate>>;

// Backend 私有异常：携带按失败语义确定的框架码，由会话入口的 catch 块返回，
// 使分类不依赖抛出位置的实现方式。
class InferletError final : public std::runtime_error {
 public:
  InferletError(int code, const std::string& message)
      : std::runtime_error(message), code_(code) {}
  int code() const noexcept { return code_; }

 private:
  int code_;
};

// 厂商调用返回非成功状态。
void CheckInferletStatus(int status, const char* operation) {
  if (status != INFERLET_OK) {
    throw InferletError(error::kInferenceFailed,
                        std::string(operation) +
                            " failed with Inferlet status " +
                            std::to_string(status));
  }
}

template <typename Interface, ExecutionProtocol protocol>
class InferletSession : public Interface {
 public:
  InferletSession(std::shared_ptr<InferletRuntime> runtime,
                  InferletHandlePtr handle)
      : runtime_(std::move(runtime)), handle_(std::move(handle)) {}

  const std::string& BackendType() const noexcept override {
    static const std::string type = InferletBackend::kBackendType;
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
  InferletInputPtr PreparePrompt(const std::string& prompt, bool add_bos) {
    if (prompt.empty() ||
        prompt.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
      throw InferletError(error::kInvalidInput,
                          "Invalid Inferlet prompt length");
    }
    // 格式化归 Model 负责。使用 token 输入以保留特殊 token 和 BOS 策略，
    // 避免再套一层厂商 chat 模板。
    int count = 0;
    const int probe = inferlet_tokenizer_encode(
        handle_.get(), prompt.data(), static_cast<int>(prompt.size()), nullptr,
        0, &count, add_bos ? 1 : 0, 1);
    if (probe != INFERLET_E_BUFFER_TOO_SMALL)
      CheckInferletStatus(probe, "Tokenize");
    if (count <= 0)
      throw InferletError(error::kInvalidOutput,
                          "Inferlet prompt produced no tokens");
    const int capacity = count;
    std::vector<int> tokens(static_cast<size_t>(capacity));
    CheckInferletStatus(
        inferlet_tokenizer_encode(
            handle_.get(), prompt.data(), static_cast<int>(prompt.size()),
            tokens.data(), capacity, &count, add_bos ? 1 : 0, 1),
        "Tokenize");
    if (count <= 0 || count > capacity)
      throw InferletError(error::kInvalidOutput,
                          "Inferlet returned invalid input token count");
    InferletInputPtr input(inferlet_task_input_allocate());
    if (!input)
      throw InferletError(error::kAllocationFailed,
                          "Inferlet task allocation failed");
    CheckInferletStatus(inferlet_task_input_set_prompt_tokens(
                            input.get(), tokens.data(), count),
                        "SetPromptTokens");
    return input;
  }

  InferletOutputPtr ExecuteTask(void* input, const GenerateOptions& options) {
    CheckInferletStatus(
        inferlet_task_input_set_max_output_tokens(input, options.max_tokens),
        "SetMaxOutputTokens");
    CheckInferletStatus(
        inferlet_task_input_set_temperature(input, options.temperature),
        "SetTemperature");
    // 框架的 0 表示不截断；Inferlet 的 0 会恢复默认 top_k=40。
    CheckInferletStatus(
        inferlet_task_input_set_top_k(
            input, options.top_k == 0 ? std::numeric_limits<int>::max()
                                      : options.top_k),
        "SetTopK");
    CheckInferletStatus(inferlet_task_input_set_top_p(input, options.top_p),
                        "SetTopP");
    CheckInferletStatus(inferlet_task_input_set_repetition_penalty(
                            input, options.repetition_penalty),
                        "SetRepetitionPenalty");
    void* raw_output = nullptr;
    const int status = inferlet_run(handle_.get(), input, &raw_output);
    InferletOutputPtr result(raw_output);
    CheckInferletStatus(status, "Run");
    if (!result)
      throw InferletError(error::kInvalidOutput,
                          "Inferlet returned a null task output");
    return result;
  }

  void RunTask(void* input, const GenerateOptions& options,
               std::string* output) {
    auto result = ExecuteTask(input, options);
    int output_token_count = 0;
    const int* output_tokens = inferlet_task_output_get_result_tokens(
        result.get(), &output_token_count);
    if (output_token_count < 0 || (output_token_count > 0 && !output_tokens)) {
      throw InferletError(error::kInvalidOutput,
                          "Inferlet returned invalid output tokens");
    }
    if (output_token_count > 0) {
      int text_size = 0;
      const int decode_probe = inferlet_tokenizer_decode(
          handle_.get(), output_tokens, output_token_count, nullptr, 0,
          &text_size, 0);
      if (decode_probe != INFERLET_E_BUFFER_TOO_SMALL)
        CheckInferletStatus(decode_probe, "Decode");
      if (text_size < 0 || text_size == std::numeric_limits<int>::max()) {
        throw InferletError(error::kInvalidOutput,
                            "Inferlet returned invalid output length");
      }
      std::string decoded(static_cast<size_t>(text_size) + 1, '\0');
      CheckInferletStatus(
          inferlet_tokenizer_decode(
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

  std::shared_ptr<InferletRuntime> runtime_;
  InferletHandlePtr handle_;
  std::mutex session_mutex_;
};

using InferletTextSessionBase =
    InferletSession<ITextGenerationSession, ExecutionProtocol::kTextGeneration>;
class InferletTextGenerationSession final : public InferletTextSessionBase {
 public:
  using InferletTextSessionBase::InferletTextSessionBase;
  int Generate(const std::string& formatted_prompt, bool add_bos,
               const GenerateOptions& options, std::optional<uint64_t> seed,
               std::string* output, std::string* diagnostic) noexcept override {
    if (diagnostic) diagnostic->clear();
    if (!output) {
      SetDiagnosticNoexcept(diagnostic, "Inferlet output pointer is null");
      return error::kInvalidArgument;
    }
    output->clear();
    if (!handle_) {
      SetDiagnosticNoexcept(diagnostic, "Inferlet handle is not initialized");
      return error::kInvalidState;
    }
    if (seed.has_value()) {
      SetDiagnosticNoexcept(
          diagnostic,
          "Inferlet does not support a fixed random seed per request");
      return error::kUnsupported;
    }
    if (formatted_prompt.empty()) {
      SetDiagnosticNoexcept(diagnostic, "Formatted prompt is empty");
      return error::kInvalidInput;
    }
    if (!text_generation::ValidateGenerateOptions(options, diagnostic)) {
      return error::kInvalidArgument;
    }

    try {
      if (formatted_prompt.size() >
          static_cast<size_t>(std::numeric_limits<int>::max())) {
        SetDiagnosticNoexcept(diagnostic,
                              "Inferlet prompt exceeds tokenizer length limit");
        return error::kInvalidInput;
      }
      std::lock_guard<std::mutex> lock(session_mutex_);
      auto input = PreparePrompt(formatted_prompt, add_bos);
      RunTask(input.get(), options, output);
      return 0;
    } catch (const InferletError& e) {
      output->clear();
      SetDiagnosticNoexcept(diagnostic, e.what());
      return e.code();
    } catch (const std::exception& e) {
      output->clear();
      SetDiagnosticNoexcept(diagnostic, e.what());
      return error::kInternalException;
    } catch (...) {
      output->clear();
      SetDiagnosticNoexcept(diagnostic,
                            "Unknown Inferlet generation exception");
      return error::kUnknownException;
    }
  }
};

using InferletEmbeddingSessionBase =
    InferletSession<IGeneratedTokenEmbeddingSession,
                    ExecutionProtocol::kGeneratedTokenEmbedding>;
class InferletGeneratedTokenEmbeddingSession final
    : public InferletEmbeddingSessionBase {
 public:
  using InferletEmbeddingSessionBase::InferletEmbeddingSessionBase;

  int GenerateEmbeddings(const std::string& prompt, bool add_bos,
                         int max_tokens, GeneratedTokenEmbeddings* output,
                         std::string* diagnostic) noexcept override {
    if (diagnostic) diagnostic->clear();
    if (!output) {
      SetDiagnosticNoexcept(diagnostic,
                            "Inferlet embedding output pointer is null");
      return error::kInvalidArgument;
    }
    *output = {};
    try {
      if (max_tokens < 1 || max_tokens > 64)
        throw InferletError(error::kInvalidArgument,
                            "Embedding max_tokens must be in [1, 64]");
      std::lock_guard<std::mutex> lock(session_mutex_);
      auto input = PreparePrompt(prompt, add_bos);
      CheckInferletStatus(inferlet_task_input_set_verbose_data_flag(
                              input.get(), INFERLET_VERBOSE_F_OUTPUT_EMBEDDING),
                          "SetVerboseDataFlag");
      CheckInferletStatus(inferlet_task_input_set_beam_size(input.get(), 1),
                          "SetBeamSize");
      GenerateOptions options;
      options.max_tokens = max_tokens;
      options.temperature = 0.0f;
      options.top_p = 1.0f;
      auto result = ExecuteTask(input.get(), options);
      int token_count = 0, rows = 0, dim = 0;
      const int* tokens =
          inferlet_task_output_get_result_tokens(result.get(), &token_count);
      const float** values =
          inferlet_task_output_get_output_embedding(result.get(), &rows, &dim);
      if (token_count < 0 || token_count > max_tokens || rows != token_count ||
          dim < 0 || dim > 65536 ||
          (rows > 0 && (!tokens || !values || dim == 0))) {
        throw InferletError(
            error::kInvalidOutput,
            "Inferlet returned missing or invalid token embeddings");
      }
      GeneratedTokenEmbeddings staged;
      for (int r = 0; r < rows; ++r) {
        if (!values[r] || tokens[r] < 0)
          throw InferletError(
              error::kInvalidOutput,
              "Inferlet returned an invalid embedding row or token");
        std::vector<float> row(values[r], values[r] + dim);
        if (!std::all_of(row.begin(), row.end(),
                         [](float v) { return std::isfinite(v); }))
          throw InferletError(error::kInvalidOutput,
                              "Inferlet returned non-finite embeddings");
        staged.token_ids.push_back(tokens[r]);
        staged.values.push_back(std::move(row));
      }
      *output = std::move(staged);
      return 0;
    } catch (const InferletError& e) {
      SetDiagnosticNoexcept(diagnostic, e.what());
      return e.code();
    } catch (const std::exception& e) {
      SetDiagnosticNoexcept(diagnostic, e.what());
      return error::kInternalException;
    } catch (...) {
      SetDiagnosticNoexcept(diagnostic, "Unknown Inferlet embedding exception");
      return error::kUnknownException;
    }
  }
};

using InferletImageSessionBase =
    InferletSession<IImageTextGenerationSession,
                    ExecutionProtocol::kImageTextGeneration>;
class InferletImageTextGenerationSession final
    : public InferletImageSessionBase {
 public:
  using InferletImageSessionBase::InferletImageSessionBase;

  int Generate(const ImageTextInput& request, const GenerateOptions& options,
               std::string* output, std::string* diagnostic) noexcept override {
    if (diagnostic) diagnostic->clear();
    if (!output) {
      SetDiagnosticNoexcept(diagnostic, "Inferlet output pointer is null");
      return error::kInvalidArgument;
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
        SetDiagnosticNoexcept(
            diagnostic,
            "Invalid image-text input dimensions, RGB planes or prompt");
        return error::kInvalidInput;
      }
      if (!text_generation::ValidateGenerateOptions(options, diagnostic))
        return error::kInvalidArgument;
      std::lock_guard<std::mutex> lock(session_mutex_);
      InferletInputPtr input(inferlet_task_input_allocate());
      if (!input)
        throw InferletError(error::kAllocationFailed,
                            "Inferlet task allocation failed");
      inferlet_multimodal_chat_content content[2]{};
      content[0].type = "image";
      content[0].data = request.rgb_chw.data();
      content[0].size = static_cast<int>(request.rgb_chw.size());
      content[0].data_type = 4;
      content[0].grid_t = 1;
      content[0].grid_h = request.height / request.patch_size;
      content[0].grid_w = request.width / request.patch_size;
      content[1].type = "text";
      content[1].data = request.prompt.c_str();
      inferlet_multimodal_chat_history history{};
      history.role = "user";
      history.content = content;
      history.length = 2;
      CheckInferletStatus(inferlet_task_input_set_multimodal_chat_history(
                              input.get(), &history, 1),
                          "SetMultiModalChatHistory");
      RunTask(input.get(), options, output);
      return 0;
    } catch (const InferletError& e) {
      output->clear();
      SetDiagnosticNoexcept(diagnostic, e.what());
      return e.code();
    } catch (const std::exception& e) {
      output->clear();
      SetDiagnosticNoexcept(diagnostic, e.what());
      return error::kInternalException;
    } catch (...) {
      output->clear();
      SetDiagnosticNoexcept(diagnostic,
                            "Unknown Inferlet image generation exception");
      return error::kUnknownException;
    }
  }
};

#endif  // HAVE_INFERLET

}  // namespace

std::shared_ptr<IBackendSession> InferletBackend::Load(
    const BackendLoadSpec& spec, std::string* diagnostic) noexcept {
  if (diagnostic) diagnostic->clear();
  try {
    if (spec.requested_protocol != ExecutionProtocol::kTextGeneration &&
        spec.requested_protocol != ExecutionProtocol::kImageTextGeneration &&
        spec.requested_protocol !=
            ExecutionProtocol::kGeneratedTokenEmbedding) {
      SetDiagnosticNoexcept(
          diagnostic,
          "Inferlet does not support requested protocol: " +
              std::string(ExecutionProtocolName(spec.requested_protocol)));
      return nullptr;
    }
    if (!ValidateExecutionTarget(spec.execution_target, diagnostic)) {
      return nullptr;
    }
#ifndef HAVE_INFERLET
    SetDiagnosticNoexcept(diagnostic,
                          "Inferlet SDK was not compiled into this build");
    return nullptr;
#else
    const auto& p = spec.Params<Params>();
    if (spec.model_file.empty()) {
      SetDiagnosticNoexcept(diagnostic, "Inferlet model path is empty");
      return nullptr;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(spec.model_file, ec) || ec) {
      SetDiagnosticNoexcept(
          diagnostic,
          "Inferlet model path is not a regular file: " + spec.model_file);
      return nullptr;
    }
    const bool image_protocol =
        spec.requested_protocol == ExecutionProtocol::kImageTextGeneration;
    nlohmann::json run_config;
    if (p.run_config_file) {
      if (!std::filesystem::is_regular_file(*p.run_config_file, ec) || ec) {
        SetDiagnosticNoexcept(
            diagnostic,
            "Inferlet run-config file does not exist: " + *p.run_config_file);
        return nullptr;
      }
      std::ifstream input(*p.run_config_file);
      if (!input)
        throw std::runtime_error("Cannot read Inferlet run-config file");
      run_config = nlohmann::json::parse(input, nullptr, false);
      if (!run_config.is_object()) {
        SetDiagnosticNoexcept(diagnostic,
                              "Inferlet run-config must be a JSON object");
        return nullptr;
      }
    }
    if (!ValidateCpuRunConfig(run_config, diagnostic)) {
      return nullptr;
    }
    if (image_protocol &&
        (!run_config.is_object() || !run_config.contains("vision"))) {
      SetDiagnosticNoexcept(
          diagnostic,
          "image_text_generation requires run-config vision.mmproj");
      return nullptr;
    }
    if (run_config.is_object() && run_config.contains("vision")) {
      const auto& vision = run_config["vision"];
      if (!vision.is_object() || !vision.contains("mmproj") ||
          !vision["mmproj"].is_string() ||
          vision["mmproj"].get<std::string>().empty()) {
        SetDiagnosticNoexcept(diagnostic, "Invalid run-config vision.mmproj");
        return nullptr;
      }
      std::filesystem::path projector;
      std::string error;
      if (!ResolveFileUnderDirectory(
              std::filesystem::path(*p.run_config_file).parent_path(),
              vision["mmproj"].get<std::string>(), &projector, &error)) {
        SetDiagnosticNoexcept(diagnostic,
                              "Invalid run-config vision.mmproj: " + error);
        return nullptr;
      }
      if (!std::filesystem::is_regular_file(projector, ec) || ec) {
        SetDiagnosticNoexcept(
            diagnostic,
            "Inferlet projector file does not exist: " + projector.string());
        return nullptr;
      }
    }

    auto runtime = std::make_shared<InferletRuntime>();
    InferletParameterPtr parameters(inferlet_parameter_allocate());
    if (!parameters) {
      SetDiagnosticNoexcept(diagnostic, "Inferlet parameter allocation failed");
      return nullptr;
    }
    inferlet_parameter_set_load_from_file_sync(parameters.get(), 1);
    if (spec.execution_target.device_id.has_value()) {
      inferlet_parameter_set_device_id(parameters.get(),
                                       *spec.execution_target.device_id);
    }
    if (p.run_config_file) {
      inferlet_parameter_set_run_config_file(parameters.get(),
                                             p.run_config_file->c_str());
    }
    InferletHandlePtr handle(
        inferlet_load_from_file(spec.model_file.c_str(), parameters.get()));
    if (!handle) {
      SetDiagnosticNoexcept(diagnostic,
                            "Inferlet model load failed: " + spec.model_file);
      return nullptr;
    }
    if (spec.requested_protocol ==
        ExecutionProtocol::kGeneratedTokenEmbedding) {
      return std::make_shared<InferletGeneratedTokenEmbeddingSession>(
          std::move(runtime), std::move(handle));
    }
    if (image_protocol) {
      return std::make_shared<InferletImageTextGenerationSession>(
          std::move(runtime), std::move(handle));
    }
    return std::make_shared<InferletTextGenerationSession>(std::move(runtime),
                                                           std::move(handle));
#endif
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(diagnostic, e.what());
    return nullptr;
  } catch (...) {
    SetDiagnosticNoexcept(diagnostic, "Unknown Inferlet load exception");
    return nullptr;
  }
}

#ifdef HAVE_INFERLET
static const BackendDefinition& InferletBackendDefinition() {
  static const BackendDefinition definition = [] {
    auto definition = MakeBackendDefinition<InferletBackend>();
    definition.description =
        "Inferlet text/image generation and generated-token embeddings (pinned "
        "CPU release; native "
        "device ID)";
    definition.supported_protocols = {
        ExecutionProtocol::kTextGeneration,
        ExecutionProtocol::kImageTextGeneration,
        ExecutionProtocol::kGeneratedTokenEmbedding};
    definition.concurrency = InferenceConcurrency::kSerialized;
    definition.params = ParamSpec();
    return definition;
  }();
  return definition;
}

REGISTER_BACKEND_WITH_DEFINITION(InferletBackend, InferletBackendDefinition());
#endif

}  // namespace llm_edgeflow

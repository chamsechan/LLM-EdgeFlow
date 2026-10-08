#include "engine/backends/llama_cpp/llama_cpp_backend.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "contracts/diagnostic.h"
#include "engine/backend_registry.h"
#include "engine/text_generation/common_autoregressive_generator.h"

#ifdef HAVE_LLAMACPP
#include "llama.h"
#endif

namespace llm_edgeflow {
namespace {

std::string NormalizePlatform(std::string platform) {
  std::transform(
      platform.begin(), platform.end(), platform.begin(),
      [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return platform;
}

#ifdef HAVE_LLAMACPP
// ① 参数结构体（默认值只在 ParamSpec 中写一次）
struct Params {
  int64_t context_size{};
  int64_t decode_batch_size{};
  int64_t n_threads{};
  int64_t n_threads_batch{};
  int64_t n_gpu_layers{};
  bool check_tensors{};
};

// ② 参数声明：名字、默认值、范围、说明和跨字段规则只写在这里
Parameters<Params> ParamSpec() {
  auto spec = Parameters<Params>(
      {Field("context_size", &Params::context_size)
           .Default(2048)
           .Range(16, 1048576)
           .Description("每次生成的上下文容量，单位为 "
                        "token，输入提示词与已生成文本共同占用。"),
       Field("decode_batch_size", &Params::decode_batch_size)
           .Default(512)
           .Range(1, 1048576)
           .Description("单次 llama_decode 提交的 token 数上限；必须不大于 "
                        "context_size。"),
       Field("n_threads", &Params::n_threads)
           .Default(0)
           .Range(0, 1024)
           .Description("token 解码的 CPU 线程数；0 保留 llama.cpp 默认值。"),
       Field("n_threads_batch", &Params::n_threads_batch)
           .Default(0)
           .Range(0, 1024)
           .Description(
               "批量 token 解码的 CPU 线程数；0 保留 llama.cpp 默认值。"),
       Field("n_gpu_layers", &Params::n_gpu_layers)
           .Default(0)
           .Range(0, 1048576)
           .Description("请求放到 GPU 的模型层数；0 使用 CPU "
                        "路径，非零须选择受支持的 GPU "
                        "执行平台。"),
       Field("check_tensors", &Params::check_tensors)
           .Default(false)
           .Description("加载权重时启用 llama.cpp 的张量数据检查。")});
  spec.Validate([](const Params& p, std::string* error) {
    if (p.decode_batch_size <= p.context_size) return true;
    SetDiagnosticNoexcept(error,
                          "decode_batch_size must not exceed context_size");
    return false;
  });
  return spec;
}

class LlamaRuntime final {
 public:
  LlamaRuntime() { llama_backend_init(); }
  ~LlamaRuntime() { llama_backend_free(); }
  LlamaRuntime(const LlamaRuntime&) = delete;
  LlamaRuntime& operator=(const LlamaRuntime&) = delete;
};

LlamaRuntime& GetLlamaRuntime() {
  static LlamaRuntime runtime;
  return runtime;
}

struct LlamaModelDeleter {
  void operator()(llama_model* model) const noexcept {
    if (model) llama_model_free(model);
  }
};

struct LlamaContextDeleter {
  void operator()(llama_context* context) const noexcept {
    if (context) llama_free(context);
  }
};

using LlamaModelPtr = std::unique_ptr<llama_model, LlamaModelDeleter>;
using LlamaContextPtr = std::unique_ptr<llama_context, LlamaContextDeleter>;

class LlamaCppDecoder final : public text_generation::IAutoregressiveDecoder {
 public:
  LlamaCppDecoder(std::shared_ptr<llama_model> model, LlamaContextPtr context,
                  size_t context_size, size_t decode_batch_size)
      : model_(std::move(model)),
        context_(std::move(context)),
        vocab_(model_ ? llama_model_get_vocab(model_.get()) : nullptr),
        context_size_(context_size),
        decode_batch_size_(decode_batch_size) {}

  int Encode(const std::string& text, bool add_bos,
             std::vector<int32_t>* tokens,
             std::string* diagnostic) noexcept override {
    try {
      if (!tokens) {
        SetDiagnosticNoexcept(diagnostic, "Token output pointer is null");
        return -1;
      }
      tokens->clear();
      if (!vocab_) {
        SetDiagnosticNoexcept(diagnostic, "llama.cpp vocabulary is null");
        return -1;
      }
      if (text.size() >
          static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        SetDiagnosticNoexcept(diagnostic,
                              "Text is too large for llama.cpp tokenizer");
        return -1;
      }

      int32_t capacity = static_cast<int32_t>(std::min<size_t>(
          text.size() + 16,
          static_cast<size_t>(std::numeric_limits<int32_t>::max())));
      std::vector<llama_token> vendor_tokens(
          static_cast<size_t>(std::max<int32_t>(capacity, 16)));
      int32_t count = llama_tokenize(
          vocab_, text.data(), static_cast<int32_t>(text.size()),
          vendor_tokens.data(), static_cast<int32_t>(vendor_tokens.size()),
          add_bos, true);
      if (count < 0) {
        vendor_tokens.resize(static_cast<size_t>(-count));
        count = llama_tokenize(
            vocab_, text.data(), static_cast<int32_t>(text.size()),
            vendor_tokens.data(), static_cast<int32_t>(vendor_tokens.size()),
            add_bos, true);
      }
      if (count < 0) {
        SetDiagnosticNoexcept(diagnostic, "llama.cpp tokenization failed");
        return -1;
      }
      vendor_tokens.resize(static_cast<size_t>(count));
      tokens->assign(vendor_tokens.begin(), vendor_tokens.end());
      return 0;
    } catch (const std::exception& e) {
      if (tokens) tokens->clear();
      SetDiagnosticNoexcept(diagnostic, e.what());
      return -1;
    } catch (...) {
      if (tokens) tokens->clear();
      SetDiagnosticNoexcept(diagnostic,
                            "Unknown llama.cpp tokenization exception");
      return -1;
    }
  }

  int DecodeToken(int32_t token, std::string* piece,
                  std::string* diagnostic) noexcept override {
    try {
      if (!piece) {
        SetDiagnosticNoexcept(diagnostic, "Decoded piece pointer is null");
        return -1;
      }
      piece->clear();
      if (!vocab_) {
        SetDiagnosticNoexcept(diagnostic, "llama.cpp vocabulary is null");
        return -1;
      }

      std::vector<char> buffer(128);
      int32_t count =
          llama_token_to_piece(vocab_, token, buffer.data(),
                               static_cast<int32_t>(buffer.size()), 0, false);
      if (count < 0) {
        buffer.resize(static_cast<size_t>(-count));
        count =
            llama_token_to_piece(vocab_, token, buffer.data(),
                                 static_cast<int32_t>(buffer.size()), 0, false);
      }
      if (count < 0) {
        SetDiagnosticNoexcept(diagnostic, "llama.cpp token decode failed");
        return -1;
      }
      piece->assign(buffer.data(), static_cast<size_t>(count));
      return 0;
    } catch (const std::exception& e) {
      if (piece) piece->clear();
      SetDiagnosticNoexcept(diagnostic, e.what());
      return -1;
    } catch (...) {
      if (piece) piece->clear();
      SetDiagnosticNoexcept(diagnostic,
                            "Unknown llama.cpp token decode exception");
      return -1;
    }
  }

  bool IsEndToken(int32_t token) const noexcept override {
    return vocab_ && llama_vocab_is_eog(vocab_, token);
  }

  size_t MaxContextTokens() const noexcept override { return context_size_; }

  int Evaluate(const std::vector<int32_t>& tokens, std::vector<float>* logits,
               std::string* diagnostic) noexcept override {
    if (!logits) {
      SetDiagnosticNoexcept(diagnostic, "Logits output pointer is null");
      return -1;
    }
    logits->clear();
    if (!model_ || !context_ || !vocab_) {
      SetDiagnosticNoexcept(diagnostic, "llama.cpp decoder is not initialized");
      return -1;
    }
    if (tokens.empty()) {
      SetDiagnosticNoexcept(diagnostic,
                            "Decoder Evaluate tokens cannot be empty");
      return -1;
    }
    try {
      if (tokens.size() >
          context_size_ - std::min(context_size_, token_count_)) {
        SetDiagnosticNoexcept(diagnostic,
                              "llama.cpp context token limit exceeded");
        return -1;
      }
      std::vector<llama_token> vendor_tokens(tokens.begin(), tokens.end());
      for (size_t offset = 0; offset < vendor_tokens.size();
           offset += decode_batch_size_) {
        const size_t chunk_size =
            std::min(decode_batch_size_, vendor_tokens.size() - offset);
        llama_batch batch = llama_batch_get_one(
            vendor_tokens.data() + offset, static_cast<int32_t>(chunk_size));
        const int32_t result = llama_decode(context_.get(), batch);
        if (result != 0) {
          SetDiagnosticNoexcept(
              diagnostic,
              "llama.cpp decode failed with code " + std::to_string(result));
          return -1;
        }
      }

      float* vendor_logits = llama_get_logits_ith(context_.get(), -1);
      const int32_t vocab_size = llama_vocab_n_tokens(vocab_);
      if (!vendor_logits || vocab_size <= 0) {
        SetDiagnosticNoexcept(diagnostic, "llama.cpp returned invalid logits");
        return -1;
      }
      logits->assign(vendor_logits, vendor_logits + vocab_size);
      token_count_ += tokens.size();
      return 0;
    } catch (const std::exception& e) {
      logits->clear();
      SetDiagnosticNoexcept(diagnostic, e.what());
      return -1;
    } catch (...) {
      logits->clear();
      SetDiagnosticNoexcept(diagnostic, "Unknown llama.cpp Evaluate exception");
      return -1;
    }
  }

 private:
  std::shared_ptr<llama_model> model_;
  LlamaContextPtr context_;
  const llama_vocab* vocab_ = nullptr;
  size_t context_size_ = 2048;
  size_t decode_batch_size_ = 512;
  size_t token_count_ = 0;
};

class LlamaCppTextGenerationSession final : public ITextGenerationSession {
 public:
  LlamaCppTextGenerationSession(std::shared_ptr<llama_model> model,
                                size_t context_size, size_t decode_batch_size,
                                int n_threads, int n_threads_batch)
      : model_(std::move(model)),
        context_size_(context_size),
        decode_batch_size_(decode_batch_size),
        n_threads_(n_threads),
        n_threads_batch_(n_threads_batch) {}

  const std::string& BackendType() const noexcept override {
    static const std::string type = LlamaCppBackend::kBackendType;
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
    try {
      if (!output) {
        SetDiagnosticNoexcept(diagnostic,
                              "Text generation output pointer is null");
        return -1;
      }
      output->clear();
      if (!model_) {
        SetDiagnosticNoexcept(diagnostic, "llama.cpp model is null");
        return -1;
      }

      std::lock_guard<std::mutex> lock(generate_mutex_);
      llama_context_params params = llama_context_default_params();
      params.n_ctx = static_cast<uint32_t>(context_size_);
      params.n_batch = static_cast<uint32_t>(decode_batch_size_);
      params.n_ubatch = static_cast<uint32_t>(decode_batch_size_);
      params.n_seq_max = 1;
      if (n_threads_ > 0) params.n_threads = n_threads_;
      if (n_threads_batch_ > 0) params.n_threads_batch = n_threads_batch_;

      LlamaContextPtr context(llama_init_from_model(model_.get(), params));
      if (!context) {
        SetDiagnosticNoexcept(diagnostic, "llama.cpp context creation failed");
        return -1;
      }
      LlamaCppDecoder decoder(model_, std::move(context), context_size_,
                              decode_batch_size_);
      return text_generation::CommonAutoregressiveGenerator::Generate(
          decoder, formatted_prompt, add_bos, options, seed, output,
          diagnostic);
    } catch (const std::exception& e) {
      output->clear();
      SetDiagnosticNoexcept(diagnostic, e.what());
      return -1;
    } catch (...) {
      output->clear();
      SetDiagnosticNoexcept(diagnostic,
                            "Unknown llama.cpp generation exception");
      return -1;
    }
  }

 private:
  std::shared_ptr<llama_model> model_;
  size_t context_size_ = 2048;
  size_t decode_batch_size_ = 512;
  int n_threads_ = 0;
  int n_threads_batch_ = 0;
  std::mutex generate_mutex_;
};

#endif  // HAVE_LLAMACPP

}  // namespace

std::shared_ptr<IBackendSession> LlamaCppBackend::Load(
    const BackendLoadSpec& spec, std::string* diagnostic) noexcept {
  try {
    if (spec.requested_protocol != ExecutionProtocol::kTextGeneration) {
      SetDiagnosticNoexcept(
          diagnostic,
          "llama.cpp backend does not support requested protocol: " +
              std::string(ExecutionProtocolName(spec.requested_protocol)));
      return nullptr;
    }
    const std::string platform =
        NormalizePlatform(spec.execution_target.platform);
    if (!platform.empty() && platform != "UNKNOWN" && platform != "CPU" &&
        platform != "CPU_GENERIC" && platform != "CUDA") {
      SetDiagnosticNoexcept(
          diagnostic,
          "llama.cpp backend does not support requested platform: " +
              spec.execution_target.platform);
      return nullptr;
    }
    const int device_id = spec.execution_target.device_id.value_or(0);
    if (device_id < 0) {
      SetDiagnosticNoexcept(diagnostic,
                            "llama.cpp device_id must be non-negative");
      return nullptr;
    }
    if ((platform == "CPU" || platform == "CPU_GENERIC") && device_id != 0) {
      SetDiagnosticNoexcept(
          diagnostic,
          "llama.cpp CPU execution only accepts device_id 0; got: " +
              std::to_string(device_id));
      return nullptr;
    }
#ifndef HAVE_LLAMACPP
    static_cast<void>(spec);
    SetDiagnosticNoexcept(diagnostic,
                          "llama.cpp backend was not compiled into this build");
    return nullptr;
#else
    // ④ 取用：参数已校验、已补默认值
    const auto& options = spec.Params<Params>();
    if (spec.model_path.empty()) {
      SetDiagnosticNoexcept(diagnostic, "llama.cpp model path is empty");
      return nullptr;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(spec.model_path, ec) || ec) {
      SetDiagnosticNoexcept(
          diagnostic, "GGUF model does not exist or is not a regular file: " +
                          spec.model_path);
      return nullptr;
    }

    if (options.n_gpu_layers == 0 && device_id != 0) {
      SetDiagnosticNoexcept(
          diagnostic,
          "llama.cpp CPU execution only accepts device_id 0; got: " +
              std::to_string(device_id));
      return nullptr;
    }
    if (options.n_gpu_layers == 0 && platform == "CUDA") {
      SetDiagnosticNoexcept(
          diagnostic, "llama.cpp CUDA execution requires n_gpu_layers > 0");
      return nullptr;
    }
    if (options.n_gpu_layers > 0 &&
        (platform == "CPU" || platform == "CPU_GENERIC")) {
      SetDiagnosticNoexcept(
          diagnostic,
          "llama.cpp n_gpu_layers requires a GPU execution platform");
      return nullptr;
    }

    (void)GetLlamaRuntime();
    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = static_cast<int32_t>(options.n_gpu_layers);
    model_params.main_gpu = device_id;
    model_params.check_tensors = options.check_tensors;
    LlamaModelPtr model(
        llama_model_load_from_file(spec.model_path.c_str(), model_params));
    if (!model) {
      SetDiagnosticNoexcept(
          diagnostic,
          "llama.cpp failed to load GGUF model: " + spec.model_path);
      return nullptr;
    }
    if (!llama_model_get_vocab(model.get())) {
      SetDiagnosticNoexcept(diagnostic, "Loaded GGUF model has no vocabulary");
      return nullptr;
    }

    std::shared_ptr<llama_model> shared_model(model.release(),
                                              LlamaModelDeleter{});
    return std::make_shared<LlamaCppTextGenerationSession>(
        std::move(shared_model), static_cast<size_t>(options.context_size),
        static_cast<size_t>(options.decode_batch_size),
        static_cast<int>(options.n_threads),
        static_cast<int>(options.n_threads_batch));
#endif
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(diagnostic, e.what());
    return nullptr;
  } catch (...) {
    SetDiagnosticNoexcept(diagnostic, "Unknown llama.cpp load exception");
    return nullptr;
  }
}

#ifdef HAVE_LLAMACPP
static const BackendDefinition kLlamaCppBackendDefinition = [] {
  auto def = MakeBackendDefinition<LlamaCppBackend>();
  def.description = "llama.cpp GGUF text-generation backend";
  def.supported_protocols = {ExecutionProtocol::kTextGeneration};
  def.concurrency = InferenceConcurrency::kSerialized;
  // ③ 登记：Definition 里只多这一行
  def.params = ParamSpec();
  return def;
}();

REGISTER_BACKEND_WITH_DEFINITION(LlamaCppBackend, kLlamaCppBackendDefinition);
#endif

}  // namespace llm_edgeflow

#include "engine/backends/llama_cpp/llama_cpp_backend.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "engine/backend_registry.h"

#ifdef HAVE_LLAMACPP
#include "llama.h"
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

std::string NormalizePlatform(std::string platform) {
  std::transform(
      platform.begin(), platform.end(), platform.begin(),
      [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return platform;
}

#ifdef HAVE_LLAMACPP

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

class LlamaCppTokenCodec final : public ITokenCodec {
 public:
  explicit LlamaCppTokenCodec(const llama_vocab* vocab) : vocab_(vocab) {}

  int Encode(const std::string& text, bool add_bos,
             std::vector<int32_t>* tokens,
             std::string* diagnostic) noexcept override {
    if (!tokens) {
      SetDiagnostic(diagnostic, "Token output pointer is null");
      return -1;
    }
    tokens->clear();
    if (!vocab_) {
      SetDiagnostic(diagnostic, "llama.cpp vocabulary is null");
      return -1;
    }
    if (text.size() >
        static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
      SetDiagnostic(diagnostic, "Text is too large for llama.cpp tokenizer");
      return -1;
    }

    try {
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
        SetDiagnostic(diagnostic, "llama.cpp tokenization failed");
        return -1;
      }
      vendor_tokens.resize(static_cast<size_t>(count));
      tokens->assign(vendor_tokens.begin(), vendor_tokens.end());
      return 0;
    } catch (const std::exception& e) {
      tokens->clear();
      SetDiagnostic(
          diagnostic,
          std::string("llama.cpp tokenization exception: ") + e.what());
      return -1;
    } catch (...) {
      tokens->clear();
      SetDiagnostic(diagnostic, "Unknown llama.cpp tokenization exception");
      return -1;
    }
  }

  int DecodeToken(int32_t token, std::string* piece,
                  std::string* diagnostic) noexcept override {
    if (!piece) {
      SetDiagnostic(diagnostic, "Decoded piece pointer is null");
      return -1;
    }
    piece->clear();
    if (!vocab_) {
      SetDiagnostic(diagnostic, "llama.cpp vocabulary is null");
      return -1;
    }

    try {
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
        SetDiagnostic(diagnostic, "llama.cpp token decode failed");
        return -1;
      }
      piece->assign(buffer.data(), static_cast<size_t>(count));
      return 0;
    } catch (const std::exception& e) {
      piece->clear();
      SetDiagnostic(
          diagnostic,
          std::string("llama.cpp token decode exception: ") + e.what());
      return -1;
    } catch (...) {
      piece->clear();
      SetDiagnostic(diagnostic, "Unknown llama.cpp token decode exception");
      return -1;
    }
  }

  bool IsEndToken(int32_t token) const noexcept override {
    return vocab_ && llama_vocab_is_eog(vocab_, token);
  }

 private:
  const llama_vocab* vocab_ = nullptr;
};

class LlamaCppSequence final : public ICausalLmSequence {
 public:
  LlamaCppSequence(std::shared_ptr<llama_model> model, LlamaContextPtr context,
                   size_t context_size, size_t decode_batch_size,
                   std::shared_ptr<std::mutex> evaluate_mutex)
      : model_(std::move(model)),
        context_(std::move(context)),
        context_size_(context_size),
        decode_batch_size_(decode_batch_size),
        evaluate_mutex_(std::move(evaluate_mutex)) {}

  int Evaluate(const std::vector<int32_t>& tokens, std::vector<float>* logits,
               std::string* diagnostic) noexcept override {
    if (!logits) {
      SetDiagnostic(diagnostic, "Logits output pointer is null");
      return -1;
    }
    logits->clear();
    if (!model_ || !context_ || !evaluate_mutex_) {
      SetDiagnostic(diagnostic, "llama.cpp sequence is not initialized");
      return -1;
    }
    if (tokens.empty()) {
      SetDiagnostic(diagnostic, "Causal LM Evaluate tokens cannot be empty");
      return -1;
    }
    try {
      std::lock_guard<std::mutex> lock(*evaluate_mutex_);
      if (tokens.size() >
          context_size_ - std::min(context_size_, token_count_)) {
        SetDiagnostic(diagnostic, "llama.cpp context token limit exceeded");
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
          SetDiagnostic(diagnostic, "llama.cpp decode failed with code " +
                                        std::to_string(result));
          return -1;
        }
      }

      float* vendor_logits = llama_get_logits_ith(context_.get(), -1);
      const llama_vocab* vocab = llama_model_get_vocab(model_.get());
      const int32_t vocab_size = vocab ? llama_vocab_n_tokens(vocab) : 0;
      if (!vendor_logits || vocab_size <= 0) {
        SetDiagnostic(diagnostic, "llama.cpp returned invalid logits");
        return -1;
      }
      logits->assign(vendor_logits, vendor_logits + vocab_size);
      token_count_ += tokens.size();
      return 0;
    } catch (const std::exception& e) {
      logits->clear();
      SetDiagnostic(diagnostic,
                    std::string("llama.cpp Evaluate exception: ") + e.what());
      return -1;
    } catch (...) {
      logits->clear();
      SetDiagnostic(diagnostic, "Unknown llama.cpp Evaluate exception");
      return -1;
    }
  }

 private:
  std::shared_ptr<llama_model> model_;
  LlamaContextPtr context_;
  size_t context_size_ = 2048;
  size_t decode_batch_size_ = 512;
  size_t token_count_ = 0;
  std::shared_ptr<std::mutex> evaluate_mutex_;
};

class LlamaCppSession final : public ICausalLmSession {
 public:
  LlamaCppSession(std::shared_ptr<llama_model> model, size_t context_size,
                  size_t decode_batch_size, int n_threads, int n_threads_batch)
      : model_(std::move(model)),
        codec_(model_ ? llama_model_get_vocab(model_.get()) : nullptr),
        context_size_(context_size),
        decode_batch_size_(decode_batch_size),
        n_threads_(n_threads),
        n_threads_batch_(n_threads_batch) {}

  const std::string& BackendType() const noexcept override {
    static const std::string type = LlamaCppBackend::kBackendType;
    return type;
  }

  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kCausalLm;
  }

  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }

  BatchPolicy GetBatchPolicy() const noexcept override {
    return BatchPolicy{1, 0};
  }

  ITokenCodec& TokenCodec() noexcept override { return codec_; }

  size_t MaxContextTokens() const noexcept override { return context_size_; }

  std::unique_ptr<ICausalLmSequence> CreateSequence(
      std::string* diagnostic) noexcept override {
    if (!model_) {
      SetDiagnostic(diagnostic, "llama.cpp model is null");
      return nullptr;
    }
    try {
      llama_context_params params = llama_context_default_params();
      params.n_ctx = static_cast<uint32_t>(context_size_);
      params.n_batch = static_cast<uint32_t>(decode_batch_size_);
      params.n_ubatch = static_cast<uint32_t>(decode_batch_size_);
      params.n_seq_max = 1;
      if (n_threads_ > 0) params.n_threads = n_threads_;
      if (n_threads_batch_ > 0) params.n_threads_batch = n_threads_batch_;

      LlamaContextPtr context(llama_init_from_model(model_.get(), params));
      if (!context) {
        SetDiagnostic(diagnostic, "llama.cpp context creation failed");
        return nullptr;
      }
      return std::make_unique<LlamaCppSequence>(
          model_, std::move(context), context_size_, decode_batch_size_,
          evaluate_mutex_);
    } catch (const std::exception& e) {
      SetDiagnostic(diagnostic,
                    std::string("llama.cpp context exception: ") + e.what());
      return nullptr;
    } catch (...) {
      SetDiagnostic(diagnostic, "Unknown llama.cpp context exception");
      return nullptr;
    }
  }

 private:
  std::shared_ptr<llama_model> model_;
  LlamaCppTokenCodec codec_;
  size_t context_size_ = 2048;
  size_t decode_batch_size_ = 512;
  int n_threads_ = 0;
  int n_threads_batch_ = 0;
  std::shared_ptr<std::mutex> evaluate_mutex_ = std::make_shared<std::mutex>();
};

#endif  // HAVE_LLAMACPP

}  // namespace

const std::string& LlamaCppBackend::BackendType() const noexcept {
  static const std::string type = kBackendType;
  return type;
}

std::shared_ptr<IBackendSession> LlamaCppBackend::Load(
    const BackendLoadSpec& spec, std::string* diagnostic) noexcept {
  if (spec.requested_protocol.has_value() &&
      *spec.requested_protocol != ExecutionProtocol::kCausalLm) {
    SetDiagnostic(
        diagnostic,
        "llama.cpp backend does not support requested protocol: " +
            std::string(ExecutionProtocolName(*spec.requested_protocol)));
    return nullptr;
  }
  const std::string platform =
      NormalizePlatform(spec.execution_target.platform);
  if (!platform.empty() && platform != "UNKNOWN" && platform != "CPU" &&
      platform != "CPU_GENERIC" && platform != "CUDA") {
    SetDiagnostic(diagnostic,
                  "llama.cpp backend does not support requested platform: " +
                      spec.execution_target.platform);
    return nullptr;
  }
  const int device_id = spec.execution_target.device_id.value_or(0);
  if (device_id < 0) {
    SetDiagnostic(diagnostic, "llama.cpp device_id must be non-negative");
    return nullptr;
  }
  if ((platform == "CPU" || platform == "CPU_GENERIC") && device_id != 0) {
    SetDiagnostic(diagnostic,
                  "llama.cpp CPU execution only accepts device_id 0; got: " +
                      std::to_string(device_id));
    return nullptr;
  }
#ifndef HAVE_LLAMACPP
  (void)spec;
  SetDiagnostic(diagnostic,
                "llama.cpp backend was not compiled into this build");
  return nullptr;
#else
  try {
    static const std::unordered_set<std::string> kAllowedFields = {
        "context_size",    "decode_batch_size", "n_threads",
        "n_threads_batch", "n_gpu_layers",      "check_tensors"};
    if (!spec.backend_config.is_object()) {
      SetDiagnostic(diagnostic, "llama.cpp backend_config must be an object");
      return nullptr;
    }
    for (const auto& [key, value] : spec.backend_config.items()) {
      (void)value;
      if (kAllowedFields.count(key) == 0) {
        SetDiagnostic(diagnostic,
                      "Unknown llama.cpp backend_config field: " + key);
        return nullptr;
      }
    }
    if (spec.model_path.empty()) {
      SetDiagnostic(diagnostic, "llama.cpp model path is empty");
      return nullptr;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(spec.model_path, ec) || ec) {
      SetDiagnostic(diagnostic,
                    "GGUF model does not exist or is not a regular file: " +
                        spec.model_path);
      return nullptr;
    }

    const int64_t context_size =
        spec.backend_config.value("context_size", int64_t{2048});
    const int64_t decode_batch_size =
        spec.backend_config.value("decode_batch_size", int64_t{512});
    const int n_threads = spec.backend_config.value("n_threads", 0);
    const int n_threads_batch = spec.backend_config.value("n_threads_batch", 0);
    const int n_gpu_layers = spec.backend_config.value("n_gpu_layers", 0);
    const bool check_tensors =
        spec.backend_config.value("check_tensors", false);

    if (context_size < 16 || context_size > 1048576 || decode_batch_size < 1 ||
        decode_batch_size > context_size || n_threads < 0 || n_threads > 1024 ||
        n_threads_batch < 0 || n_threads_batch > 1024 || n_gpu_layers < 0 ||
        n_gpu_layers > 1048576) {
      SetDiagnostic(diagnostic, "Invalid llama.cpp backend configuration");
      return nullptr;
    }

    if (n_gpu_layers == 0 && device_id != 0) {
      SetDiagnostic(diagnostic,
                    "llama.cpp CPU execution only accepts device_id 0; got: " +
                        std::to_string(device_id));
      return nullptr;
    }
    if (n_gpu_layers == 0 && platform == "CUDA") {
      SetDiagnostic(diagnostic,
                    "llama.cpp CUDA execution requires n_gpu_layers > 0");
      return nullptr;
    }
    if (n_gpu_layers > 0 && (platform == "CPU" || platform == "CPU_GENERIC")) {
      SetDiagnostic(diagnostic,
                    "llama.cpp n_gpu_layers requires a GPU execution platform");
      return nullptr;
    }

    (void)GetLlamaRuntime();
    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = n_gpu_layers;
    model_params.main_gpu = device_id;
    model_params.check_tensors = check_tensors;
    LlamaModelPtr model(
        llama_model_load_from_file(spec.model_path.c_str(), model_params));
    if (!model) {
      SetDiagnostic(diagnostic,
                    "llama.cpp failed to load GGUF model: " + spec.model_path);
      return nullptr;
    }
    if (!llama_model_get_vocab(model.get())) {
      SetDiagnostic(diagnostic, "Loaded GGUF model has no vocabulary");
      return nullptr;
    }

    std::shared_ptr<llama_model> shared_model(model.release(),
                                              LlamaModelDeleter{});
    return std::make_shared<LlamaCppSession>(
        std::move(shared_model), static_cast<size_t>(context_size),
        static_cast<size_t>(decode_batch_size), n_threads, n_threads_batch);
  } catch (const std::exception& e) {
    SetDiagnostic(diagnostic,
                  std::string("llama.cpp load exception: ") + e.what());
    return nullptr;
  } catch (...) {
    SetDiagnostic(diagnostic, "Unknown llama.cpp load exception");
    return nullptr;
  }
#endif
}

#ifdef HAVE_LLAMACPP
static const BackendDefinition kLlamaCppBackendDefinition = [] {
  BackendDefinition def;
  def.backend_type = LlamaCppBackend::kBackendType;
  def.description = "llama.cpp GGUF Causal LM inference backend";
  def.supported_protocols = {ExecutionProtocol::kCausalLm};
  def.concurrency = InferenceConcurrency::kSerialized;
  def.config_fields = {
      {"context_size", ConfigValueKind::kInteger, false, 2048, 16.0, 1048576.0},
      {"decode_batch_size", ConfigValueKind::kInteger, false, 512, 1.0,
       1048576.0},
      {"n_threads", ConfigValueKind::kInteger, false, 0, 0.0, 1024.0},
      {"n_threads_batch", ConfigValueKind::kInteger, false, 0, 0.0, 1024.0},
      {"n_gpu_layers", ConfigValueKind::kInteger, false, 0, 0.0, 1048576.0},
      {"check_tensors", ConfigValueKind::kBoolean, false, false},
  };
  return def;
}();

REGISTER_BACKEND_WITH_DEFINITION(LlamaCppBackend, kLlamaCppBackendDefinition);
#endif

}  // namespace alg_framework

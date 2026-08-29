#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "engine/inference_definition.h"

namespace alg_framework {

/**
 * @brief 推理后端会话实例基类 (持有单个加载的模型资源)
 */
class IBackendSession {
 public:
  virtual ~IBackendSession() = default;

  virtual const std::string& BackendType() const noexcept = 0;
  virtual ExecutionProtocol Protocol() const noexcept = 0;
  virtual InferenceConcurrency Concurrency() const noexcept = 0;
  virtual BatchPolicy GetBatchPolicy() const noexcept = 0;
};

/**
 * @brief Tensor Graph 执行协议会话 (ONNX Runtime, TensorRT, NPU 等)
 */
class ITensorGraphSession : public IBackendSession {
 public:
  virtual const std::vector<TensorSpec>& Inputs() const noexcept = 0;
  virtual const std::vector<TensorSpec>& Outputs() const noexcept = 0;

  virtual int Run(const TensorMap& inputs, TensorMap* outputs,
                  std::string* diagnostic = nullptr) noexcept = 0;
};

/**
 * @brief Token 编解码器接口 (由 Causal LM 后端提供分词原语)
 */
class ITokenCodec {
 public:
  virtual ~ITokenCodec() = default;

  virtual int Encode(const std::string& text, bool add_bos,
                     std::vector<int32_t>* tokens,
                     std::string* diagnostic = nullptr) noexcept = 0;
  virtual int DecodeToken(int32_t token, std::string* piece,
                          std::string* diagnostic = nullptr) noexcept = 0;
  virtual bool IsEndToken(int32_t token) const noexcept = 0;
};

/**
 * @brief 独立请求序列状态句柄 (如 KV Cache 上下文)
 */
class ISequenceState {
 public:
  virtual ~ISequenceState() = default;
};

/**
 * @brief Causal LM 执行协议会话 (llama.cpp, vLLM 等)
 */
class ICausalLmSession : public IBackendSession {
 public:
  virtual ITokenCodec& TokenCodec() noexcept = 0;
  virtual size_t MaxContextTokens() const noexcept = 0;

  virtual std::unique_ptr<ISequenceState> CreateSequence(
      std::string* diagnostic = nullptr) noexcept = 0;

  virtual int Evaluate(const std::vector<int32_t>& tokens,
                       ISequenceState& state, std::vector<float>* logits,
                       std::string* diagnostic = nullptr) noexcept = 0;
};

/**
 * @brief 后端加载参数规格
 */
struct BackendLoadSpec {
  std::string model_path;
  nlohmann::json backend_config = nlohmann::json::object();
};

/**
 * @brief 推理后端工厂/提供者纯虚接口
 */
class IInferenceBackend {
 public:
  virtual ~IInferenceBackend() = default;

  virtual const std::string& BackendType() const noexcept = 0;

  virtual std::shared_ptr<IBackendSession> Load(
      const BackendLoadSpec& spec,
      std::string* diagnostic = nullptr) noexcept = 0;
};

}  // namespace alg_framework

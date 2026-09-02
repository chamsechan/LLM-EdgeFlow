#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "contracts/inference_payloads.h"
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
 * @brief Vendor-neutral synchronous text generation session.
 *
 * The Model supplies an already formatted prompt. A concrete Backend may
 * delegate the whole operation to a managed engine or use an internal
 * autoregressive decoder and the shared sampler.
 */
class ITextGenerationSession : public IBackendSession {
 public:
  virtual int Generate(const std::string& formatted_prompt, bool add_bos,
                       const GenerateOptions& options,
                       std::optional<uint64_t> seed, std::string* output,
                       std::string* diagnostic = nullptr) noexcept = 0;
};

/**
 * @brief Vendor-neutral execution target selected by the deployment ingress.
 */
struct ExecutionTarget {
  std::optional<int> device_id;
  std::string platform;
};

/**
 * @brief 后端加载参数规格
 */
struct BackendLoadSpec {
  std::string model_path;
  nlohmann::json backend_config = nlohmann::json::object();
  std::optional<ExecutionProtocol> requested_protocol;
  ExecutionTarget execution_target;
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

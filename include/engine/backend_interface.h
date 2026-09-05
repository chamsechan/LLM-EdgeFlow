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

namespace llm_edgeflow {

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

// Model-prepared RGB planes. The patch size describes the model's spatial
// input layout; no vendor types or encoded image files cross this boundary.
struct ImageTextInput {
  std::string prompt;
  int width = 0;
  int height = 0;
  int patch_size = 0;
  std::vector<uint8_t> rgb_chw;
};

class IImageTextGenerationSession : public IBackendSession {
 public:
  virtual int Generate(const ImageTextInput& input,
                       const GenerateOptions& options, std::string* output,
                       std::string* diagnostic = nullptr) noexcept = 0;
};

// Owned, unpooled hidden states of generated tokens, in generation order.
// These are not input-token states or a sentence embedding. Each row matches
// one token_id; early EOS may return fewer rows than the requested limit.
struct GeneratedTokenEmbeddings {
  std::vector<int32_t> token_ids;
  std::vector<std::vector<float>> values;
};

class IGeneratedTokenEmbeddingSession : public IBackendSession {
 public:
  // Greedy generation, without an implicit chat template. max_tokens: 1..64.
  // Clear output on failure. Empty output is valid for immediate EOS; the
  // consuming Model decides whether that represents a usable feature.
  virtual int GenerateEmbeddings(
      const std::string& formatted_prompt, bool add_bos, int max_tokens,
      GeneratedTokenEmbeddings* output,
      std::string* diagnostic = nullptr) noexcept = 0;
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

}  // namespace llm_edgeflow

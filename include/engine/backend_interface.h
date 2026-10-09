#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "contracts/inference_payloads.h"
#include "engine/inference_definition.h"
#include "engine/tensor.h"

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
 * @brief 与厂商无关的同步文本生成会话。
 *
 * Model 提供已格式化的 prompt。具体 Backend 可以把整个操作委托给托管
 * 引擎，也可以使用内部自回归解码器和共享采样器。
 */
class ITextGenerationSession : public IBackendSession {
 public:
  virtual bool SupportsRandomSeed() const noexcept { return false; }
  virtual int Generate(const std::string& formatted_prompt, bool add_bos,
                       const GenerateOptions& options,
                       std::optional<uint64_t> seed, std::string* output,
                       std::string* diagnostic = nullptr) noexcept = 0;
};

/**
 * @brief 由部署入口选定的、与厂商无关的执行目标。
 */
struct ExecutionTarget {
  std::optional<int> device_id;
  std::string platform;
};

// Model 预处理好的 RGB 平面。patch 尺寸描述模型的空间输入布局；
// 厂商类型和编码后的图像文件均不跨越此边界。
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

// 生成 token 的隐藏状态，自有且不池化，按生成顺序排列。它们既不是输入
// token 的状态，也不是句向量。每行对应一个 token_id；提前遇到 EOS 时，
// 行数可能少于请求上限。
struct GeneratedTokenEmbeddings {
  std::vector<int32_t> token_ids;
  std::vector<std::vector<float>> values;
};

class IGeneratedTokenEmbeddingSession : public IBackendSession {
 public:
  // 贪心生成，不隐式套用 chat 模板。max_tokens 取 1..64。失败时清空输出。
  // 立即遇到 EOS 时输出为空也合法，由消费方 Model 决定其是否为可用特征。
  virtual int GenerateEmbeddings(
      const std::string& formatted_prompt, bool add_bos, int max_tokens,
      GeneratedTokenEmbeddings* output,
      std::string* diagnostic = nullptr) noexcept = 0;
};

struct AudioTranscriptionOptions {
  std::string language = "zh";
  size_t max_output_bytes = 65536;
};

class IAudioTranscriptionSession : public IBackendSession {
 public:
  virtual bool SupportsLanguage(std::string_view language) const noexcept = 0;

  virtual int Transcribe(const AudioPcmPayload& audio,
                         const AudioTranscriptionOptions& options,
                         std::string* output,
                         std::string* diagnostic = nullptr) noexcept = 0;
};

/**
 * @brief 后端加载参数规格
 */
struct BackendLoadSpec {
  explicit BackendLoadSpec(ExecutionProtocol protocol)
      : requested_protocol(protocol) {}

  std::string model_file;
  std::shared_ptr<const ParameterValues> params;
  template <typename P>
  const P& Params() const {
    if (!params) throw std::logic_error("Missing parsed parameters");
    return params->Get<P>();
  }
  ExecutionProtocol requested_protocol;
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

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/backend_interface.h"
#include "engine/backend_registry.h"

namespace alg_framework {

namespace onnxruntime_detail {

// Neutral Tensor contract helpers are intentionally vendor-type free so the
// strict boundary can be unit-tested even when ONNX Runtime is disabled.
bool ValidateInputTensor(const Tensor& tensor, const TensorSpec& spec,
                         const BatchPolicy& policy,
                         std::string* diagnostic = nullptr) noexcept;

bool ValidateOutputMetadata(ElementType element_type,
                            const std::vector<int64_t>& shape,
                            size_t runtime_element_count,
                            const TensorSpec& spec, size_t expected_batch,
                            std::string* diagnostic = nullptr) noexcept;

bool InferBatchPolicy(const std::vector<TensorSpec>& inputs,
                      const std::vector<TensorSpec>& outputs,
                      size_t configured_max_batch, BatchPolicy* policy,
                      std::string* diagnostic = nullptr) noexcept;

}  // namespace onnxruntime_detail

/**
 * @brief 基于 Microsoft ONNX Runtime 的 TensorGraph 执行协议推理后端
 *
 * 架构隔离性：
 * - 实现 Layer 4 的 IInferenceBackend 与 ITensorGraphSession 纯虚接口；
 * - 仅在 onnxruntime_backend.cpp 内部使用 onnxruntime_cxx_api.h；
 * - 上层 Model、Node、Pipeline 完全通过中性 TensorMap 交互。
 */
class OnnxRuntimeBackend : public IInferenceBackend {
 public:
  inline static constexpr char kBackendType[] = "onnxruntime";

  OnnxRuntimeBackend();
  ~OnnxRuntimeBackend() override;

  const std::string& BackendType() const noexcept override;

  std::shared_ptr<IBackendSession> Load(
      const BackendLoadSpec& spec,
      std::string* diagnostic = nullptr) noexcept override;
};

}  // namespace alg_framework

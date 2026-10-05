#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/backend_identity.h"
#include "engine/backend_interface.h"
#include "engine/backend_registry.h"

namespace llm_edgeflow {

namespace onnxruntime_detail {

// 中性 Tensor 契约辅助函数有意不含厂商类型，
// 以便在禁用 ONNX Runtime 时也能单测严格边界。
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
 * - 实现模型执行层的 IInferenceBackend 与 ITensorGraphSession 纯虚接口；
 * - 仅在 onnxruntime_backend.cpp 内部使用 onnxruntime_cxx_api.h；
 * - 上层 Model、Node、Pipeline 完全通过中性 TensorMap 交互。
 */
class OnnxRuntimeBackend : public BackendIdentity<OnnxRuntimeBackend> {
 public:
  inline static constexpr char kBackendType[] = "onnxruntime";

  OnnxRuntimeBackend();
  ~OnnxRuntimeBackend() override;

  std::shared_ptr<IBackendSession> Load(
      const BackendLoadSpec& spec,
      std::string* diagnostic = nullptr) noexcept override;
};

}  // namespace llm_edgeflow

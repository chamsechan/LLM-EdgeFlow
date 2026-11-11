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

/**
 * @brief 在借用输入缓冲区交给 ONNX Runtime
 * 前，核对类型、形状、批策略及精确字节数。
 * 静态维必须匹配模型声明，运行时维不能为负，首维必须符合批策略。
 * 不读取张量元素；校验失败通过 diagnostic 说明原因并返回 false。
 */
bool ValidateInputTensor(const Tensor& tensor, const TensorSpec& spec,
                         const BatchPolicy& policy,
                         std::string* diagnostic = nullptr) noexcept;

/**
 * @brief 在复制厂商输出前核对类型、静态维、元素总数及本次执行的批大小。
 * expected_batch 为 0 时不额外约束首维；所有运行时维仍须非负。
 * 元数据与模型声明或调用规模不一致时返回 false。
 */
bool ValidateOutputMetadata(ElementType element_type,
                            const std::vector<int64_t>& shape,
                            size_t runtime_element_count,
                            const TensorSpec& spec, size_t expected_batch,
                            std::string* diagnostic = nullptr) noexcept;

/**
 * @brief 从全部输入输出的首维推导统一批策略，拒绝互相冲突的静态批大小。
 * 存在静态首维时采用该固定批大小；全部动态时使用 configured_max_batch。
 * 失败时重置 policy 并返回 false。
 */
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

  /**
   * @brief 加载 ONNX 图并读取输入输出元数据，推导 Session 的固定或动态批策略。
   * 返回的 Session 持有运行环境和图资源；协议、目标设备或元数据不支持时失败。
   * 未编入 ONNX Runtime 或加载异常时返回空指针并提供诊断。
   */
  std::shared_ptr<IBackendSession> Load(
      const BackendLoadSpec& spec,
      std::string* diagnostic = nullptr) noexcept override;
};

}  // namespace llm_edgeflow

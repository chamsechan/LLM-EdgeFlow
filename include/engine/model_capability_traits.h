#pragma once

#include "engine/model_interface.h"

namespace llm_edgeflow {

/**
 * @brief 中性模型能力特征萃取器 (SSOT Model Capability Traits)
 * 未知接口无默认映射，若尝试使用未知模型接口将在编译期失败。
 */
template <typename ModelInterface>
struct ModelCapabilityTraits;

template <>
struct ModelCapabilityTraits<ILlmModel> {
  static constexpr const char* Capability() noexcept { return "llm"; }
};

template <>
struct ModelCapabilityTraits<IEmbeddingModel> {
  static constexpr const char* Capability() noexcept { return "embedding"; }
};

template <>
struct ModelCapabilityTraits<IRerankModel> {
  static constexpr const char* Capability() noexcept { return "rerank"; }
};

template <>
struct ModelCapabilityTraits<IOcrModel> {
  static constexpr const char* Capability() noexcept { return "ocr"; }
};

template <>
struct ModelCapabilityTraits<IAsrModel> {
  static constexpr const char* Capability() noexcept { return "asr"; }
};

}  // namespace llm_edgeflow

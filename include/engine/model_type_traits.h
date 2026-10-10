#pragma once

#include "engine/model_interface.h"

namespace llm_edgeflow {

/**
 * @brief 模型类别由所实现的中性接口推导
 * 未知接口无默认映射，若尝试使用未知模型接口将在编译期失败。
 */
template <typename ModelInterface>
struct ModelTypeTraits;

template <>
struct ModelTypeTraits<ILlmModel> {
  static constexpr const char* ModelType() noexcept { return "llm"; }
};

template <>
struct ModelTypeTraits<IEmbeddingModel> {
  static constexpr const char* ModelType() noexcept { return "embedding"; }
};

template <>
struct ModelTypeTraits<IRerankModel> {
  static constexpr const char* ModelType() noexcept { return "rerank"; }
};

template <>
struct ModelTypeTraits<IOcrModel> {
  static constexpr const char* ModelType() noexcept { return "ocr"; }
};

template <>
struct ModelTypeTraits<IAsrModel> {
  static constexpr const char* ModelType() noexcept { return "asr"; }
};

}  // namespace llm_edgeflow

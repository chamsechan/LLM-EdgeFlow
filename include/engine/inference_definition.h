#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "contracts/config_schema.h"

namespace llm_edgeflow {

/**
 * @brief 底层执行协议枚举
 */
enum class ExecutionProtocol {
  kTensorGraph,
  kTextGeneration,
  kImageTextGeneration,
  kGeneratedTokenEmbedding,
  kAudioTranscription,
};

/**
 * @brief 推理并发模型
 */
enum class InferenceConcurrency {
  kSerialized,
  kConcurrent,
};

inline bool IsValidExecutionProtocol(ExecutionProtocol protocol) noexcept {
  switch (protocol) {
    case ExecutionProtocol::kTensorGraph:
    case ExecutionProtocol::kTextGeneration:
    case ExecutionProtocol::kImageTextGeneration:
    case ExecutionProtocol::kGeneratedTokenEmbedding:
    case ExecutionProtocol::kAudioTranscription:
      return true;
    default:
      return false;
  }
}

inline bool IsValidInferenceConcurrency(
    InferenceConcurrency concurrency) noexcept {
  switch (concurrency) {
    case InferenceConcurrency::kSerialized:
    case InferenceConcurrency::kConcurrent:
      return true;
    default:
      return false;
  }
}

inline bool IsConcurrencyCompatible(InferenceConcurrency declared,
                                    InferenceConcurrency actual) noexcept {
  if (!IsValidInferenceConcurrency(declared) ||
      !IsValidInferenceConcurrency(actual)) {
    return false;
  }
  return declared != InferenceConcurrency::kConcurrent ||
         actual == InferenceConcurrency::kConcurrent;
}

/**
 * @brief 批处理调度策略
 */
struct BatchPolicy {
  size_t max_batch_size = 1;
  size_t fixed_batch_size = 0;  // 0 表示可变执行 batch，>0 表示必须为固定 batch
};

/**
 * @brief 模型语义定义元数据 (ModelDefinition)
 */
struct ModelDefinition {
  std::string model_type;
  std::string capability;
  std::string description;
  ExecutionProtocol required_protocol = ExecutionProtocol::kTensorGraph;
  std::vector<ConfigFieldDefinition> config_fields;
  InferenceConcurrency concurrency = InferenceConcurrency::kSerialized;
};

/**
 * @brief 推理后端定义元数据 (BackendDefinition)
 */
struct BackendDefinition {
  std::string backend_type;
  std::string description;
  std::vector<ExecutionProtocol> supported_protocols;
  std::vector<ConfigFieldDefinition> config_fields;
  InferenceConcurrency concurrency = InferenceConcurrency::kSerialized;
  // Pure validation of normalized config; no session allocation or external
  // I/O.
  std::function<bool(const nlohmann::json&, std::string*)> validate_config;
};

inline const char* ExecutionProtocolName(ExecutionProtocol protocol) noexcept {
  switch (protocol) {
    case ExecutionProtocol::kTensorGraph:
      return "tensor_graph";
    case ExecutionProtocol::kTextGeneration:
      return "text_generation";
    case ExecutionProtocol::kImageTextGeneration:
      return "image_text_generation";
    case ExecutionProtocol::kGeneratedTokenEmbedding:
      return "generated_token_embedding";
    case ExecutionProtocol::kAudioTranscription:
      return "audio_transcription";
    default:
      return "unknown";
  }
}

inline const char* InferenceConcurrencyName(
    InferenceConcurrency conc) noexcept {
  switch (conc) {
    case InferenceConcurrency::kSerialized:
      return "serialized";
    case InferenceConcurrency::kConcurrent:
      return "concurrent";
    default:
      return "unknown";
  }
}

}  // namespace llm_edgeflow

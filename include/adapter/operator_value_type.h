#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "adapter/biz_input_constraints.h"
#include "adapter/operator_io_contracts.h"

namespace llm_edgeflow {

/**
 * @brief 输入限制配置
 */
struct ResolvedInputLimits {
  size_t max_text_bytes = 64 * 1024;             // 64 KiB
  size_t max_doc_text_bytes = 10 * 1024 * 1024;  // 10 MiB
  size_t max_image_uri_bytes = 4096;             // 4 KiB
  int32_t max_audio_pcm_samples =
      biz_input::kMaxAudioPcmSamples;                         // 960k samples
  size_t max_audio_pcm_bytes = biz_input::kMaxAudioPcmBytes;  // 10 MiB
  int32_t min_sample_rate = biz_input::kMinSampleRate;
  int32_t max_sample_rate = biz_input::kMaxSampleRate;
  int32_t max_rerank_candidates = 8;
  size_t max_buffer_bytes = 10 * 1024 * 1024;  // 10 MiB
  size_t max_any_bytes = 10 * 1024 * 1024;     // 10 MiB
};

struct OutputCapacityFieldConfig {
  uint32_t default_capacity = 0;
  uint32_t max_capacity = 0;
};

using ComputeOutputBlockPayloadBytesFn = std::function<bool(
    const ResolvedOutputPoolSpec& spec, size_t* out_bytes, std::string* err)>;

/**
 * @brief Operator 输出类型的容量与内存布局契约
 */
struct OperatorOutputLayoutDescriptor {
  // 每个字段对应输出镜像结构中的一个 CompanyString 指针。
  std::unordered_map<std::string, OutputCapacityFieldConfig>
      string_capacity_fields;
  uint32_t max_metadata_elements = 0;
  ComputeOutputBlockPayloadBytesFn compute_block_payload_bytes;
};

/**
 * @brief 移动语义清理动作 (无需 std::function 堆分配或控制块)
 */
struct CleanupAction {
  void* ptr = nullptr;
  void (*deleter)(void*) noexcept = nullptr;

  void Execute() noexcept {
    if (ptr && deleter) {
      deleter(ptr);
      ptr = nullptr;
    }
  }
};

/**
 * @brief 由输出池持有所有权的外部结构块 (具备完整 RAII 自动回滚与类型安全清理)
 */
struct OwnedExternalBlock {
  void* raw_struct = nullptr;
  std::vector<CleanupAction> cleanups;

  OwnedExternalBlock() = default;
  ~OwnedExternalBlock() { Destroy(); }

  OwnedExternalBlock(OwnedExternalBlock&& other) noexcept
      : raw_struct(other.raw_struct), cleanups(std::move(other.cleanups)) {
    other.raw_struct = nullptr;
  }

  OwnedExternalBlock& operator=(OwnedExternalBlock&& other) noexcept {
    if (this != &other) {
      Destroy();
      raw_struct = other.raw_struct;
      cleanups = std::move(other.cleanups);
      other.raw_struct = nullptr;
    }
    return *this;
  }

  OwnedExternalBlock(const OwnedExternalBlock&) = delete;
  OwnedExternalBlock& operator=(const OwnedExternalBlock&) = delete;

  // Register each allocation while its unique_ptr still guards failure.
  template <typename T>
  T* Own(std::unique_ptr<T> value) {
    T* raw = value.get();
    cleanups.push_back(
        {raw, [](void* ptr) noexcept { delete static_cast<T*>(ptr); }});
    value.release();
    return raw;
  }

  template <typename T>
  T* OwnArray(std::unique_ptr<T[]> value) {
    T* raw = value.get();
    cleanups.push_back(
        {raw, [](void* ptr) noexcept { delete[] static_cast<T*>(ptr); }});
    value.release();
    return raw;
  }

  void Destroy() noexcept {
    for (auto it = cleanups.rbegin(); it != cleanups.rend(); ++it) {
      it->Execute();
    }
    cleanups.clear();
    raw_struct = nullptr;
  }
};

using ValidateExternalFn = std::function<int(
    const void* ptr, const ResolvedInputLimits& limits, std::string* err)>;

using AllocateExternalFn =
    std::function<int(const ResolvedOutputPoolSpec& spec,
                      OwnedExternalBlock* out_block, std::string* err)>;

using ResetExternalFn =
    std::function<void(void* ptr, const ResolvedOutputPoolSpec& spec)>;

using DestroyExternalFn = std::function<void(OwnedExternalBlock* block)>;

/**
 * @brief Operator 值类型绑定描述符
 */
using NormalizeOutputParametersFn = std::function<bool(
    const std::string& requested,
    std::shared_ptr<const OutputAllocationParameters>* normalized,
    std::string* error)>;

// Parse only this structure's parameters, once during Create. Parser has the
// signature bool(const std::string&, T*, std::string*). T is an ordinary
// struct; the framework handles immutable ownership and checked access
// thereafter.
template <typename T, typename Parser>
NormalizeOutputParametersFn MakeOutputParameterParser(Parser parse) {
  return [parse = std::move(parse)](
             const std::string& text,
             std::shared_ptr<const OutputAllocationParameters>* normalized,
             std::string* error) {
    if (!normalized) return false;
    normalized->reset();
    T parameters{};
    if (!parse(text, &parameters, error)) return false;
    *normalized =
        std::make_shared<const detail::TypedOutputAllocationParameters<T>>(
            std::move(parameters));
    return true;
  };
}

struct OperatorValueTypeBinding {
  std::string canonical_suffix;
  std::string external_c_type_name;
  IoDirection direction = IoDirection::kUnknown;
  OperatorOutputLayoutDescriptor output_layout;
  ValidateExternalFn validate_external;
  AllocateExternalFn allocate_external;
  ResetExternalFn reset_external;
  DestroyExternalFn destroy_external;
  // Empty for a ValueType's default allocation; assigned by allocator
  // registration.
  std::string allocation_name;
  // Called at configuration time to create immutable single-object parameters.
  // Prefer MakeOutputParameterParser<T> with an ordinary parameter struct.
  // Parameter destruction must not allocate; no file or queue access here.
  NormalizeOutputParametersFn normalize_parameters;
};

// Source-extension registration. All registrations finish before Operator Init.
// A named allocator preserves the registered outer type, while defining its
// own nested layout, parameter normalization, byte accounting and cleanup.
bool RegisterOperatorValueType(const OperatorValueTypeBinding& binding);
bool RegisterOperatorOutputAllocator(const std::string& name,
                                     const OperatorValueTypeBinding& binding);

bool NormalizeOutputParameters(
    const OperatorValueTypeBinding& binding, const std::string& requested,
    std::shared_ptr<const OutputAllocationParameters>* normalized,
    std::string* error) noexcept;

#define REGISTER_OPERATOR_VALUE_TYPE(RegisterFn)       \
  namespace {                                          \
  const bool g_operator_value_type_##RegisterFn = [] { \
    RegisterFn();                                      \
    return true;                                       \
  }();                                                 \
  }

#define REGISTER_OPERATOR_OUTPUT_ALLOCATOR(RegisterFn)       \
  namespace {                                                \
  const bool g_operator_output_allocator_##RegisterFn = [] { \
    RegisterFn();                                            \
    return true;                                             \
  }();                                                       \
  }

}  // namespace llm_edgeflow

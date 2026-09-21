#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "adapter/biz_input_constraints.h"
#include "adapter/operator_io_contracts.h"
#include "edgeflow/operator/types.h"

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

namespace operator_value_detail {
CompanyString* AllocateNestedCompanyString(uint32_t capacity,
                                           OwnedExternalBlock* block);
CompanyAny* AllocateNestedCompanyAny(uint32_t count, int32_t type_id,
                                     OwnedExternalBlock* block);
void ResetNestedCompanyString(CompanyString* value) noexcept;
void ResetNestedCompanyAny(CompanyAny* value) noexcept;
bool ComputeStandardOutputBlockPayloadBytes(size_t root_bytes,
                                            const ResolvedOutputPoolSpec& spec,
                                            size_t* bytes,
                                            std::string* error) noexcept;
}  // namespace operator_value_detail

// Keep the external null diagnostic and type erasure at the binding boundary.
template <typename T, typename Validate>
OperatorValueTypeBinding MakeTypedInputBinding(const char* suffix,
                                               const char* type_name,
                                               Validate validate) {
  OperatorValueTypeBinding binding;
  binding.canonical_suffix = suffix;
  binding.external_c_type_name = type_name;
  binding.direction = IoDirection::kInput;
  binding.validate_external = [type_name = std::string(type_name), validate](
                                  const void* ptr,
                                  const ResolvedInputLimits& limits,
                                  std::string* err) -> int {
    if (!ptr) {
      if (err) *err = std::string(type_name) + " pointer is null";
      return -3;
    }
    return validate(*static_cast<const T*>(ptr), limits, err);
  };
  return binding;
}

// Each pooled string is declared once for capacity validation, allocation and
// reset. Member pointers keep the descriptor tied to its concrete C structure.
template <typename T>
struct OutputStringField {
  std::string name;
  CompanyString* T::*member;
  OutputCapacityFieldConfig capacity;
};

template <typename T, typename ResetScalars>
OperatorValueTypeBinding MakePooledOutputBinding(
    const char* suffix, const char* type_name,
    std::vector<OutputStringField<T>> string_fields, ResetScalars reset_scalars,
    CompanyAny* T::*metadata_field = nullptr,
    uint32_t max_metadata_elements = 0) {
  static_assert(std::is_nothrow_invocable_v<ResetScalars, T&>);
  OperatorValueTypeBinding binding;
  binding.canonical_suffix = suffix;
  binding.external_c_type_name = type_name;
  binding.direction = IoDirection::kOutput;
  for (size_t i = 0; i < string_fields.size(); ++i) {
    const auto& field = string_fields[i];
    if (field.name.empty() || field.member == nullptr ||
        !binding.output_layout.string_capacity_fields
             .emplace(field.name, field.capacity)
             .second) {
      throw std::invalid_argument(
          "Pooled output requires unique named string fields");
    }
    for (size_t j = 0; j < i; ++j) {
      if (string_fields[j].member == field.member) {
        throw std::invalid_argument(
            "Pooled output string member is declared twice");
      }
    }
  }
  if (!metadata_field && max_metadata_elements != 0) {
    throw std::invalid_argument("Metadata capacity requires a metadata member");
  }
  binding.output_layout.max_metadata_elements = max_metadata_elements;
  binding.output_layout.compute_block_payload_bytes =
      [](const ResolvedOutputPoolSpec& spec, size_t* out_bytes,
         std::string* err) noexcept {
        return operator_value_detail::ComputeStandardOutputBlockPayloadBytes(
            sizeof(T), spec, out_bytes, err);
      };
  binding.allocate_external = [string_fields, metadata_field, reset_scalars](
                                  const ResolvedOutputPoolSpec& spec,
                                  OwnedExternalBlock* block,
                                  std::string*) -> int {
    // Reserve before allocating: one root plus a wrapper and data buffer
    // for each nested field. OwnedExternalBlock rolls back partial failure.
    block->cleanups.reserve(1 + 2 * string_fields.size() +
                            (metadata_field ? 2 : 0));
    auto* raw = block->Own(std::make_unique<T>());
    reset_scalars(*raw);
    for (const auto& field : string_fields) {
      raw->*field.member = operator_value_detail::AllocateNestedCompanyString(
          spec.GetCapacity(field.name), block);
    }
    if (metadata_field) {
      raw->*metadata_field = operator_value_detail::AllocateNestedCompanyAny(
          spec.meta_num, spec.metadata_type_id, block);
    }
    block->raw_struct = raw;
    return 0;
  };
  binding.reset_external = [string_fields, metadata_field, reset_scalars](
                               void* ptr,
                               const ResolvedOutputPoolSpec&) noexcept {
    if (!ptr) return;
    auto* raw = static_cast<T*>(ptr);
    // Reset values only; nested storage and metadata type survive reuse.
    reset_scalars(*raw);
    for (const auto& field : string_fields) {
      operator_value_detail::ResetNestedCompanyString(raw->*field.member);
    }
    if (metadata_field)
      operator_value_detail::ResetNestedCompanyAny(raw->*metadata_field);
  };
  binding.destroy_external = [](OwnedExternalBlock* block) noexcept {
    if (block) block->Destroy();
  };
  return binding;
}

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

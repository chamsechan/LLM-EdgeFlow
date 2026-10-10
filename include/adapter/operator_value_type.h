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

#include "adapter/input_limits.h"
#include "adapter/operator_io_contracts.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {

/**
 * @brief 宿主输入的安全上限
 *
 * 值类型在读取宿主内存前用这些上限检查指针与长度；默认值全部来自
 * input_limits 常量，Operator
 * 不提供配置项。测试和自定义值类型可传入更小的上限。
 */
struct InputLimits {
  size_t max_text_bytes = input_limits::kMaxTextBytes;
  size_t max_doc_text_bytes = input_limits::kMaxDocTextBytes;
  size_t max_image_uri_bytes = input_limits::kMaxImageUriBytes;
  int32_t max_audio_pcm_samples =
      input_limits::kMaxAudioPcmSamples;  // 96 万个采样点
  size_t max_audio_pcm_bytes = input_limits::kMaxAudioPcmBytes;  // 10 MiB
  int32_t min_sample_rate = input_limits::kMinSampleRate;
  int32_t max_sample_rate = input_limits::kMaxSampleRate;
  size_t max_buffer_bytes = input_limits::kMaxBufferBytes;
  size_t max_any_bytes = input_limits::kMaxAnyBytes;
};

struct OutputCapacityFieldConfig {
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

  // 在 unique_ptr 仍负责失败回收时登记每次分配。
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
    const void* ptr, const InputLimits& limits, std::string* err)>;

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

// 只解析本结构的参数，由 Converter Audit 每个注册归一化一次并缓存。Parser
// 签名为 bool(const std::string&, T*, std::string*)。T 为普通 struct，
// 此后由框架负责不可变所有权和受检访问。
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
  std::function<uint64_t(const void*)> read_request_id;
  std::function<int32_t(const void*)> read_service_type;
  std::function<void(void*, int32_t)> write_service_type;
  AllocateExternalFn allocate_external;
  ResetExternalFn reset_external;
  DestroyExternalFn destroy_external;
  // ValueType 默认分配时为空；由分配器注册时赋值。
  std::string allocation_name;
  // 配置期调用，创建不可变的单对象参数。建议使用普通参数 struct 和
  // MakeOutputParameterParser<T>。参数析构不得分配内存，
  // 此处也不得访问文件或队列。
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

// 宿主结构名取自 DECLARE_EXTERNAL_TYPE_TRAITS，登记处不再重复书写。
template <typename T>
constexpr const char* HostTypeName() {
  static_assert(ExternalTypeTraits<T>::TypeName()[0] != '\0',
                "Declare the host struct with DECLARE_EXTERNAL_TYPE_TRAITS "
                "before registering its ValueType");
  return ExternalTypeTraits<T>::TypeName();
}

template <typename T>
void SetRequestIdMember(OperatorValueTypeBinding* binding,
                        uint64_t T::*member) {
  if (!binding || !member)
    throw std::invalid_argument("Invalid request ID member");
  binding->read_request_id = [member](const void* value) {
    return static_cast<const T*>(value)->*member;
  };
}

template <typename T>
void SetServiceTypeMember(OperatorValueTypeBinding* binding,
                          int32_t T::*member) {
  if (!binding || !member)
    throw std::invalid_argument("Invalid service type member");
  binding->read_service_type = [member](const void* value) {
    return static_cast<const T*>(value)->*member;
  };
  if (binding->direction == IoDirection::kOutput) {
    binding->write_service_type = [member](void* value, int32_t service) {
      static_cast<T*>(value)->*member = service;
    };
  }
}

// 外部空值诊断和类型擦除保留在绑定边界。
template <typename T, typename Validate>
OperatorValueTypeBinding MakeTypedInputBinding(const char* suffix,
                                               Validate validate) {
  OperatorValueTypeBinding binding;
  binding.canonical_suffix = suffix;
  binding.external_c_type_name = HostTypeName<T>();
  binding.direction = IoDirection::kInput;
  binding.validate_external = [validate](const void* ptr,
                                         const InputLimits& limits,
                                         std::string* err) -> int {
    if (!ptr) {
      if (err) *err = std::string(HostTypeName<T>()) + " pointer is null";
      return -3;
    }
    return validate(*static_cast<const T*>(ptr), limits, err);
  };
  return binding;
}

// 每个池化字符串只声明一次，用于容量校验、分配和重置。
// 成员指针使描述符与具体的 C 结构绑定。
template <typename T>
struct OutputStringField {
  std::string name;
  CompanyString* T::*member;
  OutputCapacityFieldConfig capacity;
};

template <typename T, typename ResetScalars>
OperatorValueTypeBinding MakePooledOutputBinding(
    const char* suffix, std::vector<OutputStringField<T>> string_fields,
    ResetScalars reset_scalars, CompanyAny* T::*metadata_field = nullptr,
    uint32_t max_metadata_elements = 0) {
  static_assert(std::is_nothrow_invocable_v<ResetScalars, T&>);
  OperatorValueTypeBinding binding;
  binding.canonical_suffix = suffix;
  binding.external_c_type_name = HostTypeName<T>();
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
    // 分配前先预留：一个根块，每个嵌套字段再加一个包装和一个数据缓冲区。
    // OwnedExternalBlock 负责回滚部分失败。
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
    // 只重置值；嵌套存储和元数据类型在复用时保留。
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

// 源码扩展注册，须在 Operator Init 前全部完成。具名分配器保留已注册的
// 外层类型，同时自定义嵌套布局、参数归一化、字节计数和清理。
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

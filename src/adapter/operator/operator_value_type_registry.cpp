#include "adapter/operator/operator_value_type_registry.h"

#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>

#include "adapter/operator/operator_output_pool.h"
#include "company_alg_interface.h"
#include "operator/company_operator_types.h"

namespace llm_edgeflow {

namespace {

std::atomic<int> g_alloc_failure_countdown{-1};
std::atomic<OperatorValueTypeRegistry::RegistryExceptionInjectPoint>
    g_registry_inject_point{
        OperatorValueTypeRegistry::RegistryExceptionInjectPoint::kNone};

void CheckAllocFailureProbe() {
  if (g_alloc_failure_countdown.load() >= 0) {
    if (g_alloc_failure_countdown.fetch_sub(1) == 0) {
      throw std::bad_alloc();
    }
  }
}

const CompanyAnyTypeDescriptor kBuiltinAnyTypes[] = {
    {0, 0, 1, "none"},
    {1, sizeof(float), alignof(float), "float32"},
    {2, sizeof(int32_t), alignof(int32_t), "int32"},
    {3, sizeof(uint8_t), alignof(uint8_t), "uint8"},
    {4, sizeof(int64_t), alignof(int64_t), "int64"},
    {5, sizeof(double), alignof(double), "float64"},
};

inline void DeleteCharArray(void* p) noexcept {
  OutputPoolState::RecordDestroyed();
  delete[] static_cast<char*>(p);
}

inline void DeleteCompanyString(void* p) noexcept {
  OutputPoolState::RecordDestroyed();
  delete static_cast<CompanyString*>(p);
}

inline void DeleteCompanyAny(void* p) noexcept {
  OutputPoolState::RecordDestroyed();
  delete static_cast<CompanyAny*>(p);
}

inline void DeleteAnyPayload(void* p) noexcept {
  OutputPoolState::RecordDestroyed();
  delete[] static_cast<uint8_t*>(p);
}

template <typename T>
inline void DeleteTypedObject(void* p) noexcept {
  OutputPoolState::RecordDestroyed();
  delete static_cast<T*>(p);
}

void RegisterCleanup(OwnedExternalBlock* block, CleanupAction action);

template <typename T>
T* AllocateRootOutput(size_t cleanup_capacity, OwnedExternalBlock* block) {
  if (OutputPoolState::GetFailureStageProbe() ==
      OutputPoolState::FailureStage::kRootStructAlloc) {
    return nullptr;
  }
  block->cleanups.reserve(cleanup_capacity);
  CheckAllocFailureProbe();
  std::unique_ptr<T, decltype(&DeleteTypedObject<T>)> holder(
      new T(), DeleteTypedObject<T>);
  OutputPoolState::RecordConstructed();
  T* raw = holder.get();
  RegisterCleanup(block, {raw, DeleteTypedObject<T>});
  holder.release();
  return raw;
}

void DestroyExternalBlock(OwnedExternalBlock* block) noexcept {
  if (block) block->Destroy();
}

void RegisterCleanup(OwnedExternalBlock* block, CleanupAction action) {
  if (OutputPoolState::GetFailureStageProbe() ==
          OutputPoolState::FailureStage::kCleanupRegister &&
      static_cast<int>(block->cleanups.size()) ==
          OutputPoolState::GetTargetBlockIndex()) {
    throw std::bad_alloc();
  }
  block->cleanups.push_back(action);
}

CompanyString* AllocateNestedCompanyString(uint32_t capacity,
                                           OwnedExternalBlock* block) {
  if (OutputPoolState::GetFailureStageProbe() ==
      OutputPoolState::FailureStage::kNestedStringAlloc) {
    throw std::bad_alloc();
  }
  CheckAllocFailureProbe();
  std::unique_ptr<CompanyString, decltype(&DeleteCompanyString)> str(
      new CompanyString(), DeleteCompanyString);
  OutputPoolState::RecordConstructed();
  CheckAllocFailureProbe();
  size_t alloc_bytes = static_cast<size_t>(capacity) + 1;
  std::unique_ptr<char[], decltype(&DeleteCharArray)> data(
      new char[alloc_bytes], DeleteCharArray);
  OutputPoolState::RecordConstructed();
  std::memset(data.get(), 0, alloc_bytes);

  str->length = 0;
  str->data = data.get();

  char* raw_data = data.get();
  CompanyString* raw_str = str.get();
  RegisterCleanup(block, {raw_data, DeleteCharArray});
  data.release();
  RegisterCleanup(block, {raw_str, DeleteCompanyString});
  str.release();
  return raw_str;
}

CompanyAny* AllocateNestedCompanyAny(uint32_t meta_num, int32_t type_id,
                                     OwnedExternalBlock* block) {
  if (meta_num == 0 || type_id == 0) {
    return nullptr;
  }
  const auto* desc = FindCompanyAnyType(type_id);
  if (!desc || desc->element_size == 0) {
    return nullptr;
  }
  if (OutputPoolState::GetFailureStageProbe() ==
      OutputPoolState::FailureStage::kNestedAnyAlloc) {
    throw std::bad_alloc();
  }
  CheckAllocFailureProbe();
  std::unique_ptr<CompanyAny, decltype(&DeleteCompanyAny)> any(
      new CompanyAny(), DeleteCompanyAny);
  OutputPoolState::RecordConstructed();
  size_t total_bytes = 0;
  if (!CheckedMultiply(meta_num, desc->element_size, &total_bytes)) {
    return nullptr;
  }
  CheckAllocFailureProbe();
  std::unique_ptr<uint8_t[], decltype(&DeleteAnyPayload)> data(
      new uint8_t[total_bytes], DeleteAnyPayload);
  OutputPoolState::RecordConstructed();
  std::memset(data.get(), 0, total_bytes);

  any->type_id = type_id;
  any->element_count = 0;
  any->byte_length = 0;
  any->data = data.get();

  uint8_t* raw_data = data.get();
  CompanyAny* raw_any = any.get();
  RegisterCleanup(block, {raw_data, DeleteAnyPayload});
  data.release();
  RegisterCleanup(block, {raw_any, DeleteCompanyAny});
  any.release();
  return raw_any;
}

void ResetNestedCompanyString(CompanyString* str) noexcept {
  if (str) {
    str->length = 0;
    if (str->data) {
      str->data[0] = '\0';
    }
  }
}

void ResetNestedCompanyAny(CompanyAny* any) noexcept {
  if (any) {
    any->element_count = 0;
    any->byte_length = 0;
  }
}

OperatorValueTypeBinding MakeInputBinding(
    std::string canonical_suffix, std::string external_c_type_name,
    ValidateExternalFn validate_external) {
  OperatorValueTypeBinding binding;
  binding.canonical_suffix = std::move(canonical_suffix);
  binding.external_c_type_name = std::move(external_c_type_name);
  binding.direction = IoDirection::kInput;
  binding.validate_external = std::move(validate_external);
  return binding;
}

bool ComputeStandardOutputBlockPayloadBytes(size_t root_struct_bytes,
                                            const ResolvedOutputPoolSpec& spec,
                                            size_t* out_bytes,
                                            std::string* err) noexcept {
  if (!out_bytes) {
    if (err) *err = "Null output block payload pointer";
    return false;
  }
  *out_bytes = 0;

  size_t block_bytes = root_struct_bytes;
  for (const auto& [field, capacity] : spec.capacities) {
    size_t string_bytes = 0;
    if (!CheckedAdd(static_cast<size_t>(capacity), 1, &string_bytes) ||
        !CheckedAdd(string_bytes, sizeof(CompanyString), &string_bytes) ||
        !CheckedAdd(block_bytes, string_bytes, &block_bytes)) {
      if (err) {
        *err = spec.type + " capacity calculation overflowed for field '" +
               field + "'";
      }
      return false;
    }
  }

  if (spec.meta_num > 0) {
    const auto* metadata_desc = FindCompanyAnyType(spec.metadata_type_id);
    if (!metadata_desc || metadata_desc->element_size == 0) {
      if (err) *err = "Metadata type is not registered";
      return false;
    }
    size_t metadata_payload = 0;
    size_t metadata_bytes = 0;
    if (!CheckedMultiply(spec.meta_num, metadata_desc->element_size,
                         &metadata_payload) ||
        !CheckedAdd(metadata_payload, sizeof(CompanyAny), &metadata_bytes) ||
        !CheckedAdd(block_bytes, metadata_bytes, &block_bytes)) {
      if (err) *err = spec.type + " metadata calculation overflowed";
      return false;
    }
  }

  *out_bytes = block_bytes;
  return true;
}

OperatorValueTypeBinding MakeOutputBinding(
    std::string canonical_suffix, std::string external_c_type_name,
    std::unordered_map<std::string, OutputCapacityFieldConfig>
        string_capacity_fields,
    uint32_t max_metadata_elements, size_t root_struct_bytes,
    AllocateExternalFn allocate_external, ResetExternalFn reset_external) {
  OperatorValueTypeBinding binding;
  binding.canonical_suffix = std::move(canonical_suffix);
  binding.external_c_type_name = std::move(external_c_type_name);
  binding.direction = IoDirection::kOutput;
  binding.output_layout.string_capacity_fields =
      std::move(string_capacity_fields);
  binding.output_layout.max_metadata_elements = max_metadata_elements;
  binding.output_layout.compute_block_payload_bytes =
      [root_struct_bytes](const ResolvedOutputPoolSpec& spec, size_t* out_bytes,
                          std::string* err) noexcept {
        return ComputeStandardOutputBlockPayloadBytes(root_struct_bytes, spec,
                                                      out_bytes, err);
      };
  binding.allocate_external = std::move(allocate_external);
  binding.reset_external = std::move(reset_external);
  binding.destroy_external = DestroyExternalBlock;
  return binding;
}

}  // namespace

bool ResolveOutputPoolSpec(const OperatorValueTypeBinding& binding,
                           const ResolvedOutputPoolSpec& requested,
                           ResolvedOutputPoolSpec* resolved,
                           std::string* err) noexcept {
  try {
    if (!resolved) {
      if (err) *err = "Null resolved output pool spec pointer";
      return false;
    }
    *resolved = ResolvedOutputPoolSpec{};

    if (binding.direction != IoDirection::kOutput ||
        binding.canonical_suffix.empty()) {
      if (err) *err = "Value type binding is not a valid output binding";
      return false;
    }
    if (requested.type.empty() || requested.type != binding.canonical_suffix) {
      if (err) {
        *err = "Output pool spec type '" + requested.type +
               "' does not match binding suffix '" + binding.canonical_suffix +
               "'";
      }
      return false;
    }

    ResolvedOutputPoolSpec candidate = requested;
    for (const auto& [field, capacity] : requested.capacities) {
      const auto schema_it =
          binding.output_layout.string_capacity_fields.find(field);
      if (schema_it == binding.output_layout.string_capacity_fields.end()) {
        if (err) {
          *err = "Unknown capacity field '" + field + "' for output type '" +
                 binding.canonical_suffix + "'";
        }
        return false;
      }
      if (capacity == 0 || capacity > schema_it->second.max_capacity) {
        if (err) {
          *err = "Capacity for field '" + field + "' (" +
                 std::to_string(capacity) +
                 ") is zero or exceeds max hard limit (" +
                 std::to_string(schema_it->second.max_capacity) + ")";
        }
        return false;
      }
    }
    for (const auto& [field, field_config] :
         binding.output_layout.string_capacity_fields) {
      if (candidate.capacities.find(field) == candidate.capacities.end()) {
        candidate.capacities[field] = field_config.default_capacity;
      }
    }

    if ((candidate.meta_num == 0) != (candidate.metadata_type_id == 0)) {
      if (err) *err = "Metadata count and type must both be zero or non-zero";
      return false;
    }
    if (candidate.meta_num > binding.output_layout.max_metadata_elements) {
      if (err) {
        *err = "Metadata count " + std::to_string(candidate.meta_num) +
               " exceeds max limit " +
               std::to_string(binding.output_layout.max_metadata_elements) +
               " for output type '" + binding.canonical_suffix + "'";
      }
      return false;
    }
    if (candidate.meta_num > 0 &&
        !FindCompanyAnyType(candidate.metadata_type_id)) {
      if (err) {
        *err = "Metadata type " + std::to_string(candidate.metadata_type_id) +
               " is invalid or not whitelisted";
      }
      return false;
    }

    *resolved = std::move(candidate);
    return true;
  } catch (const std::exception& e) {
    if (err) {
      *err = std::string("Exception resolving output pool spec: ") + e.what();
    }
    return false;
  } catch (...) {
    if (err) *err = "Unknown exception resolving output pool spec";
    return false;
  }
}

bool ComputeOutputPoolPayloadBytes(const OperatorValueTypeBinding& binding,
                                   const ResolvedOutputPoolSpec& spec,
                                   uint32_t depth, size_t* out_bytes,
                                   std::string* err) noexcept {
  if (!out_bytes) {
    if (err) *err = "Null out_bytes pointer";
    return false;
  }
  *out_bytes = 0;

  ResolvedOutputPoolSpec resolved;
  if (!ResolveOutputPoolSpec(binding, spec, &resolved, err)) {
    return false;
  }

  uint32_t effective_depth = (depth == 0) ? kDefaultOutputPoolDepth : depth;
  if (effective_depth > kMaxOutputPoolDepth) {
    if (err) {
      *err = "Output pool depth " + std::to_string(effective_depth) +
             " exceeds max limit " + std::to_string(kMaxOutputPoolDepth);
    }
    return false;
  }

  if (!binding.output_layout.compute_block_payload_bytes) {
    if (err) {
      *err = "Missing output block budget callback for suffix '" +
             binding.canonical_suffix + "'";
    }
    return false;
  }

  size_t single_block_bytes = 0;
  try {
    if (!binding.output_layout.compute_block_payload_bytes(
            resolved, &single_block_bytes, err)) {
      return false;
    }
  } catch (const std::exception& e) {
    if (err) {
      *err = "Exception computing output block payload for suffix '" +
             binding.canonical_suffix + "': " + e.what();
    }
    return false;
  } catch (...) {
    if (err) {
      *err = "Unknown exception computing output block payload for suffix '" +
             binding.canonical_suffix + "'";
    }
    return false;
  }
  if (single_block_bytes == 0) {
    if (err) {
      *err = "Output block payload is zero for suffix '" +
             binding.canonical_suffix + "'";
    }
    return false;
  }

  size_t total_pool_bytes = 0;
  if (!CheckedMultiply(effective_depth, single_block_bytes,
                       &total_pool_bytes)) {
    if (err) *err = "Total pool payload calculation overflowed";
    return false;
  }

  if (total_pool_bytes > kMaxHandlePoolPayloadBytes) {
    if (err) {
      *err = "Total pool payload (" + std::to_string(total_pool_bytes) +
             " bytes) exceeds maximum allowed payload budget " +
             std::to_string(kMaxHandlePoolPayloadBytes) + " bytes (64 MiB)";
    }
    return false;
  }

  *out_bytes = total_pool_bytes;
  return true;
}

bool ComputeOutputPoolPayloadBytes(const std::string& suffix,
                                   const ResolvedOutputPoolSpec& spec,
                                   uint32_t depth, size_t* out_bytes,
                                   std::string* err) noexcept {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix(suffix);
  if (!binding || binding->canonical_suffix != suffix) {
    if (out_bytes) *out_bytes = 0;
    if (err) {
      *err = "Unknown or non-canonical output suffix '" + suffix + "'";
    }
    return false;
  }
  return ComputeOutputPoolPayloadBytes(*binding, spec, depth, out_bytes, err);
}

void OperatorValueTypeRegistry::SetAllocationFailureCountdown(
    int count) noexcept {
  g_alloc_failure_countdown.store(count);
}

int OperatorValueTypeRegistry::GetAllocationFailureCountdown() noexcept {
  return g_alloc_failure_countdown.load();
}

void OperatorValueTypeRegistry::SetExceptionInjectPoint(
    RegistryExceptionInjectPoint point) noexcept {
  g_registry_inject_point.store(point);
}

OperatorValueTypeRegistry::RegistryExceptionInjectPoint
OperatorValueTypeRegistry::GetExceptionInjectPoint() noexcept {
  return g_registry_inject_point.load();
}

const CompanyAnyTypeDescriptor* FindCompanyAnyType(int32_t type_id) noexcept {
  for (const auto& item : kBuiltinAnyTypes) {
    if (item.type_id == type_id) {
      return &item;
    }
  }
  return nullptr;
}

OperatorValueTypeRegistry& OperatorValueTypeRegistry::Instance() {
  static OperatorValueTypeRegistry instance;
  return instance;
}

bool OperatorValueTypeRegistry::ParseKey(const std::string& key,
                                         std::string* out_namespace,
                                         std::string* out_suffix) noexcept {
  if (key.empty()) return false;
  size_t last_dot = key.rfind('.');
  if (last_dot == std::string::npos || last_dot == 0 ||
      last_dot == key.size() - 1) {
    return false;
  }
  if (out_namespace) {
    *out_namespace = key.substr(0, last_dot);
  }
  if (out_suffix) {
    *out_suffix = key.substr(last_dot + 1);
  }
  return true;
}

int OperatorValueTypeRegistry::ValidateCompanyString(
    const CompanyString* str, size_t max_bytes, const char* field_name,
    std::string* err) noexcept {
  if (!str) {
    if (err) *err = std::string(field_name) + " pointer is null";
    return -3;
  }
  if (str->length < 0) {
    if (err)
      *err = std::string(field_name) + " has negative length " +
             std::to_string(str->length);
    return -3;
  }
  if (static_cast<size_t>(str->length) > max_bytes) {
    if (err)
      *err = std::string(field_name) + " length " +
             std::to_string(str->length) + " exceeds max limit " +
             std::to_string(max_bytes);
    return -3;
  }
  if (str->length > 0) {
    if (!str->data) {
      if (err)
        *err = std::string(field_name) + " length is " +
               std::to_string(str->length) + " but data pointer is null";
      return -3;
    }
    for (int32_t i = 0; i < str->length; ++i) {
      if (str->data[i] == '\0') {
        if (err)
          *err = std::string(field_name) +
                 " contains forbidden embedded NUL at byte offset " +
                 std::to_string(i);
        return -3;
      }
    }
  }
  return 0;
}

int OperatorValueTypeRegistry::ValidateCompanyBuffer(
    const CompanyBuffer* buf, size_t max_bytes, const char* field_name,
    std::string* err) noexcept {
  if (!buf) {
    if (err) *err = std::string(field_name) + " pointer is null";
    return -3;
  }
  if (buf->length < 0) {
    if (err)
      *err = std::string(field_name) + " has negative length " +
             std::to_string(buf->length);
    return -3;
  }
  if (static_cast<size_t>(buf->length) > max_bytes) {
    if (err)
      *err = std::string(field_name) + " length exceeds max limit " +
             std::to_string(max_bytes);
    return -3;
  }
  if (buf->length > 0 && !buf->data) {
    if (err) *err = std::string(field_name) + " data pointer is null";
    return -3;
  }
  return 0;
}

int OperatorValueTypeRegistry::ValidateCompanyAnyPayload(
    const CompanyAny* any, size_t max_any_bytes, const char* field_name,
    std::string* err) noexcept {
  if (!any) {
    if (err) *err = std::string(field_name) + " pointer is null";
    return -3;
  }
  if (any->element_count < 0 || any->byte_length < 0) {
    if (err) {
      *err = std::string(field_name) + " has negative count or length: count=" +
             std::to_string(any->element_count) +
             ", length=" + std::to_string(any->byte_length);
    }
    return -3;
  }
  if (static_cast<size_t>(any->byte_length) > max_any_bytes) {
    if (err) {
      *err = std::string(field_name) + " byte_length " +
             std::to_string(any->byte_length) + " exceeds max limit " +
             std::to_string(max_any_bytes);
    }
    return -3;
  }
  if (any->type_id == 0) {
    if (any->element_count != 0 || any->byte_length != 0) {
      if (err) {
        *err = std::string(field_name) +
               " has type_id=0 but non-zero count/length";
      }
      return -3;
    }
    return 0;
  }

  const auto* desc = FindCompanyAnyType(any->type_id);
  if (!desc) {
    if (err) {
      *err = std::string(field_name) +
             " has unknown or unwhitelisted type_id " +
             std::to_string(any->type_id);
    }
    return -3;
  }

  size_t expected_bytes = 0;
  if (!CheckedMultiply(static_cast<size_t>(any->element_count),
                       desc->element_size, &expected_bytes)) {
    if (err) {
      *err =
          std::string(field_name) + " element_count multiplication overflowed";
    }
    return -3;
  }
  if (expected_bytes != static_cast<size_t>(any->byte_length)) {
    if (err) {
      *err = std::string(field_name) + " size equation mismatch: expected " +
             std::to_string(expected_bytes) + " bytes for " +
             std::to_string(any->element_count) + " elements of type " +
             desc->debug_name + ", but byte_length is " +
             std::to_string(any->byte_length);
    }
    return -3;
  }
  if (any->byte_length > 0 && !any->data) {
    if (err) {
      *err = std::string(field_name) + " non-empty payload has null data";
    }
    return -3;
  }
  return 0;
}

bool OperatorValueTypeRegistry::HasConflict() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_conflict_;
}

int OperatorValueTypeRegistry::GlobalInit() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (has_conflict_) {
    return -6;
  }
  if (audited_) {
    return 0;
  }
  for (const auto& [suffix, binding] : bindings_by_canonical_) {
    (void)suffix;
    if (binding.canonical_suffix.empty() ||
        binding.external_c_type_name.empty()) {
      has_conflict_ = true;
      return -6;
    }
    if (binding.direction == IoDirection::kOutput) {
      if (!binding.allocate_external || !binding.reset_external ||
          !binding.destroy_external ||
          !binding.output_layout.compute_block_payload_bytes) {
        has_conflict_ = true;
        return -6;
      }
      for (const auto& [field, config] :
           binding.output_layout.string_capacity_fields) {
        if (field.empty() || config.default_capacity == 0 ||
            config.max_capacity < config.default_capacity) {
          has_conflict_ = true;
          return -6;
        }
      }
    } else if (binding.direction == IoDirection::kInput) {
      if (!binding.validate_external) {
        has_conflict_ = true;
        return -6;
      }
    } else {
      has_conflict_ = true;
      return -6;
    }
  }
  audited_ = true;
  return 0;
}

bool OperatorValueTypeRegistry::RegisterBinding(
    const OperatorValueTypeBinding& binding) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (audited_) {
    return false;
  }
  if (binding.canonical_suffix.empty() ||
      binding.external_c_type_name.empty()) {
    has_conflict_ = true;
    return false;
  }

  // 检查 canonical 是否与已有 canonical 冲突
  if (bindings_by_canonical_.find(binding.canonical_suffix) !=
      bindings_by_canonical_.end()) {
    has_conflict_ = true;
    return false;
  }

  // 原子预检通过后，通过 Copy-and-Swap 一次性提交
  try {
    if (g_registry_inject_point.load() ==
        OperatorValueTypeRegistry::RegistryExceptionInjectPoint::
            kCopyCanonicalMap) {
      throw std::runtime_error("Injected failure copying canonical map");
    }
    auto temp_bindings = bindings_by_canonical_;

    if (g_registry_inject_point.load() ==
        OperatorValueTypeRegistry::RegistryExceptionInjectPoint::
            kCanonicalInsert) {
      throw std::runtime_error("Injected failure on canonical insert");
    }
    temp_bindings[binding.canonical_suffix] = binding;

    if (g_registry_inject_point.load() ==
        OperatorValueTypeRegistry::RegistryExceptionInjectPoint::kPublish) {
      throw std::runtime_error("Injected failure before registry publish");
    }
    bindings_by_canonical_.swap(temp_bindings);
  } catch (...) {
    // 资源分配或复制异常不污染契约冲突状态，原快照保持不变
    return false;
  }
  return true;
}

const OperatorValueTypeBinding* OperatorValueTypeRegistry::GetBindingBySuffix(
    const std::string& suffix) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it_can = bindings_by_canonical_.find(suffix);
  if (it_can != bindings_by_canonical_.end()) {
    return &it_can->second;
  }
  return nullptr;
}

OperatorValueTypeRegistry::OperatorValueTypeRegistry() {
  RegisterBuiltinBindings();
}

void OperatorValueTypeRegistry::RegisterBuiltinBindings() {
  // 1. string -> CompanyString
  RegisterBinding(MakeInputBinding(
      "string", "CompanyString",
      [](const void* ptr, const ResolvedInputLimits& limits,
         std::string* err) -> int {
        if (!ptr) {
          if (err) *err = "CompanyString pointer is null";
          return -3;
        }
        return ValidateCompanyString(static_cast<const CompanyString*>(ptr),
                                     limits.max_text_bytes, "string", err);
      }));

  // 2. buffer -> CompanyBuffer
  RegisterBinding(MakeInputBinding(
      "buffer", "CompanyBuffer",
      [](const void* ptr, const ResolvedInputLimits& limits,
         std::string* err) -> int {
        if (!ptr) {
          if (err) *err = "CompanyBuffer pointer is null";
          return -3;
        }
        return ValidateCompanyBuffer(static_cast<const CompanyBuffer*>(ptr),
                                     limits.max_buffer_bytes, "buffer", err);
      }));

  // 3. any -> CompanyAny
  RegisterBinding(MakeInputBinding(
      "any", "CompanyAny",
      [](const void* ptr, const ResolvedInputLimits& limits,
         std::string* err) -> int {
        if (!ptr) {
          if (err) *err = "CompanyAny pointer is null";
          return -3;
        }
        return ValidateCompanyAnyPayload(static_cast<const CompanyAny*>(ptr),
                                         limits.max_any_bytes, "any", err);
      }));

  // 4. frame -> CompanyFrame
  RegisterBinding(MakeInputBinding(
      "frame", "CompanyFrame",
      [](const void* ptr, const ResolvedInputLimits& limits,
         std::string* err) -> int {
        if (!ptr) {
          if (err) *err = "CompanyFrame pointer is null";
          return -3;
        }
        const auto* f = static_cast<const CompanyFrame*>(ptr);
        if (!f->image_uri) {
          if (err) *err = "CompanyFrame.image_uri is null";
          return -3;
        }
        int ret =
            ValidateCompanyString(f->image_uri, limits.max_image_uri_bytes,
                                  "CompanyFrame.image_uri", err);
        if (ret != 0) return ret;
        if (f->metadata) {
          ret = ValidateCompanyAnyPayload(f->metadata, limits.max_any_bytes,
                                          "CompanyFrame.metadata", err);
          if (ret != 0) return ret;
        }
        return 0;
      }));

  // 5. od_out -> CompanyOdOutput
  RegisterBinding(MakeOutputBinding(
      "od_out", "CompanyOdOutput", {{"result_json", {2047, 65536}}}, 65536,
      sizeof(CompanyOdOutput),
      [](const ResolvedOutputPoolSpec& spec, OwnedExternalBlock* out_block,
         std::string* /*err*/) -> int {
        auto* raw = AllocateRootOutput<CompanyOdOutput>(5, out_block);
        if (!raw) return -4;

        raw->request_id = 0;
        raw->detected_box_count = 0;
        raw->status_code = 0;
        raw->result_json = AllocateNestedCompanyString(
            spec.GetCapacity("result_json"), out_block);
        raw->metadata = AllocateNestedCompanyAny(
            spec.meta_num, spec.metadata_type_id, out_block);
        out_block->raw_struct = raw;
        return 0;
      },
      [](void* ptr, const ResolvedOutputPoolSpec& /*spec*/) noexcept {
        if (!ptr) return;
        auto* raw = static_cast<CompanyOdOutput*>(ptr);
        raw->request_id = 0;
        raw->detected_box_count = 0;
        raw->status_code = 0;
        ResetNestedCompanyString(raw->result_json);
        ResetNestedCompanyAny(raw->metadata);
      }));

  // 6. keyword_in -> CompanyOperatorKeywordInput
  RegisterBinding(MakeInputBinding(
      "keyword_in", "CompanyOperatorKeywordInput",
      [](const void* ptr, const ResolvedInputLimits& limits,
         std::string* err) -> int {
        if (!ptr) {
          if (err) *err = "CompanyOperatorKeywordInput pointer is null";
          return -3;
        }
        const auto* in = static_cast<const CompanyOperatorKeywordInput*>(ptr);
        if (!in->sentence_text) {
          if (err) *err = "sentence_text pointer is null";
          return -3;
        }
        return ValidateCompanyString(in->sentence_text, limits.max_text_bytes,
                                     "sentence_text", err);
      }));

  // 7. keyword_out -> CompanyOperatorKeywordOutput
  RegisterBinding(MakeOutputBinding(
      "keyword_out", "CompanyOperatorKeywordOutput",
      {{"match_result_json", {2047, 65536}}}, 0,
      sizeof(CompanyOperatorKeywordOutput),
      [](const ResolvedOutputPoolSpec& spec, OwnedExternalBlock* out_block,
         std::string* /*err*/) -> int {
        auto* raw =
            AllocateRootOutput<CompanyOperatorKeywordOutput>(3, out_block);
        if (!raw) return -4;

        raw->request_id = 0;
        raw->is_hit = 0;
        raw->match_result_json = AllocateNestedCompanyString(
            spec.GetCapacity("match_result_json"), out_block);
        out_block->raw_struct = raw;
        return 0;
      },
      [](void* ptr, const ResolvedOutputPoolSpec& /*spec*/) noexcept {
        if (!ptr) return;
        auto* raw = static_cast<CompanyOperatorKeywordOutput*>(ptr);
        raw->request_id = 0;
        raw->is_hit = 0;
        ResetNestedCompanyString(raw->match_result_json);
      }));

  // 8. entity_in -> CompanyOperatorEntityInput
  RegisterBinding(MakeInputBinding(
      "entity_in", "CompanyOperatorEntityInput",
      [](const void* ptr, const ResolvedInputLimits& limits,
         std::string* err) -> int {
        if (!ptr) {
          if (err) *err = "CompanyOperatorEntityInput pointer is null";
          return -3;
        }
        const auto* in = static_cast<const CompanyOperatorEntityInput*>(ptr);
        if (!in->sentence_text) {
          if (err) *err = "sentence_text pointer is null";
          return -3;
        }
        return ValidateCompanyString(in->sentence_text, limits.max_text_bytes,
                                     "sentence_text", err);
      }));

  // 9. entity_out -> CompanyOperatorEntityOutput
  RegisterBinding(MakeOutputBinding(
      "entity_out", "CompanyOperatorEntityOutput",
      {{"entities_json", {2047, 65536}}}, 0,
      sizeof(CompanyOperatorEntityOutput),
      [](const ResolvedOutputPoolSpec& spec, OwnedExternalBlock* out_block,
         std::string* /*err*/) -> int {
        auto* raw =
            AllocateRootOutput<CompanyOperatorEntityOutput>(3, out_block);
        if (!raw) return -4;

        raw->request_id = 0;
        raw->status_code = 0;
        raw->entities_json = AllocateNestedCompanyString(
            spec.GetCapacity("entities_json"), out_block);
        out_block->raw_struct = raw;
        return 0;
      },
      [](void* ptr, const ResolvedOutputPoolSpec& /*spec*/) noexcept {
        if (!ptr) return;
        auto* raw = static_cast<CompanyOperatorEntityOutput*>(ptr);
        raw->request_id = 0;
        raw->status_code = 0;
        ResetNestedCompanyString(raw->entities_json);
      }));

  // 10. doc_in -> CompanyOperatorDocInput
  RegisterBinding(MakeInputBinding(
      "doc_in", "CompanyOperatorDocInput",
      [](const void* ptr, const ResolvedInputLimits& limits,
         std::string* err) -> int {
        if (!ptr) {
          if (err) *err = "CompanyOperatorDocInput pointer is null";
          return -3;
        }
        const auto* in = static_cast<const CompanyOperatorDocInput*>(ptr);
        if (!in->query_text) {
          if (err) *err = "query_text pointer is null";
          return -3;
        }
        int ret = ValidateCompanyString(in->query_text, limits.max_text_bytes,
                                        "query_text", err);
        if (ret != 0) return ret;
        if (in->doc_text) {
          ret = ValidateCompanyString(in->doc_text, limits.max_doc_text_bytes,
                                      "doc_text", err);
          if (ret != 0) return ret;
        }
        return 0;
      }));

  // 11. doc_out -> CompanyOperatorDocOutput
  RegisterBinding(MakeOutputBinding(
      "doc_out", "CompanyOperatorDocOutput",
      {{"intent_name", {63, 255}}, {"answer_text", {1023, 65536}}}, 0,
      sizeof(CompanyOperatorDocOutput),
      [](const ResolvedOutputPoolSpec& spec, OwnedExternalBlock* out_block,
         std::string* /*err*/) -> int {
        auto* raw = AllocateRootOutput<CompanyOperatorDocOutput>(5, out_block);
        if (!raw) return -4;

        raw->request_id = 0;
        raw->confidence = 0.0f;
        raw->chunk_count = 0;
        raw->status_code = 0;
        raw->intent_name = AllocateNestedCompanyString(
            spec.GetCapacity("intent_name"), out_block);
        raw->answer_text = AllocateNestedCompanyString(
            spec.GetCapacity("answer_text"), out_block);
        out_block->raw_struct = raw;
        return 0;
      },
      [](void* ptr, const ResolvedOutputPoolSpec& /*spec*/) noexcept {
        if (!ptr) return;
        auto* raw = static_cast<CompanyOperatorDocOutput*>(ptr);
        raw->request_id = 0;
        raw->confidence = 0.0f;
        raw->chunk_count = 0;
        raw->status_code = 0;
        ResetNestedCompanyString(raw->intent_name);
        ResetNestedCompanyString(raw->answer_text);
      }));

  // 12. audit_in -> CompanyOperatorAuditInput
  RegisterBinding(MakeInputBinding(
      "audit_in", "CompanyOperatorAuditInput",
      [](const void* ptr, const ResolvedInputLimits& limits,
         std::string* err) -> int {
        if (!ptr) {
          if (err) *err = "CompanyOperatorAuditInput pointer is null";
          return -3;
        }
        const auto* in = static_cast<const CompanyOperatorAuditInput*>(ptr);
        if (!in->user_text) {
          if (err) *err = "user_text pointer is null";
          return -3;
        }
        int ret = ValidateCompanyString(in->user_text, limits.max_text_bytes,
                                        "user_text", err);
        if (ret != 0) return ret;
        if (in->channel_name) {
          ret =
              ValidateCompanyString(in->channel_name, 256, "channel_name", err);
          if (ret != 0) return ret;
        }
        return 0;
      }));

  // 13. audit_out -> CompanyOperatorAuditOutput
  RegisterBinding(MakeOutputBinding(
      "audit_out", "CompanyOperatorAuditOutput",
      {{"risk_level", {31, 255}},
       {"matched_policy_clause", {255, 4096}},
       {"audit_verdict_json", {1023, 65536}}},
      0, sizeof(CompanyOperatorAuditOutput),
      [](const ResolvedOutputPoolSpec& spec, OwnedExternalBlock* out_block,
         std::string* /*err*/) -> int {
        auto* raw =
            AllocateRootOutput<CompanyOperatorAuditOutput>(7, out_block);
        if (!raw) return -4;

        raw->request_id = 0;
        raw->risk_score = 0.0f;
        raw->status_code = 0;
        raw->risk_level = AllocateNestedCompanyString(
            spec.GetCapacity("risk_level"), out_block);
        raw->matched_policy_clause = AllocateNestedCompanyString(
            spec.GetCapacity("matched_policy_clause"), out_block);
        raw->audit_verdict_json = AllocateNestedCompanyString(
            spec.GetCapacity("audit_verdict_json"), out_block);
        out_block->raw_struct = raw;
        return 0;
      },
      [](void* ptr, const ResolvedOutputPoolSpec& /*spec*/) noexcept {
        if (!ptr) return;
        auto* raw = static_cast<CompanyOperatorAuditOutput*>(ptr);
        raw->request_id = 0;
        raw->risk_score = 0.0f;
        raw->status_code = 0;
        ResetNestedCompanyString(raw->risk_level);
        ResetNestedCompanyString(raw->matched_policy_clause);
        ResetNestedCompanyString(raw->audit_verdict_json);
      }));

  // 14. audio_in -> CompanyOperatorAudioInput
  RegisterBinding(MakeInputBinding(
      "audio_in", "CompanyOperatorAudioInput",
      [](const void* ptr, const ResolvedInputLimits& limits,
         std::string* err) -> int {
        if (!ptr) {
          if (err) *err = "CompanyOperatorAudioInput pointer is null";
          return -3;
        }
        const auto* in = static_cast<const CompanyOperatorAudioInput*>(ptr);
        if (in->sample_rate < limits.min_sample_rate ||
            in->sample_rate > limits.max_sample_rate) {
          if (err) {
            *err = "sample_rate " + std::to_string(in->sample_rate) +
                   " out of valid range [" +
                   std::to_string(limits.min_sample_rate) + ", " +
                   std::to_string(limits.max_sample_rate) + "]";
          }
          return -3;
        }
        if (in->pcm_length <= 0 ||
            in->pcm_length > limits.max_audio_pcm_samples) {
          if (err)
            *err = "pcm_length " + std::to_string(in->pcm_length) +
                   " invalid or exceeds limit " +
                   std::to_string(limits.max_audio_pcm_samples);
          return -3;
        }
        if (!in->pcm_buffer) {
          if (err) *err = "pcm_buffer pointer is null";
          return -3;
        }
        return 0;
      }));

  // 15. audio_out -> CompanyOperatorAudioOutput
  RegisterBinding(MakeOutputBinding(
      "audio_out", "CompanyOperatorAudioOutput",
      {{"transcribed_text", {511, 16384}}, {"intent_slot_json", {1023, 65536}}},
      0, sizeof(CompanyOperatorAudioOutput),
      [](const ResolvedOutputPoolSpec& spec, OwnedExternalBlock* out_block,
         std::string* /*err*/) -> int {
        auto* raw =
            AllocateRootOutput<CompanyOperatorAudioOutput>(5, out_block);
        if (!raw) return -4;

        raw->request_id = 0;
        raw->status_code = 0;
        raw->transcribed_text = AllocateNestedCompanyString(
            spec.GetCapacity("transcribed_text"), out_block);
        raw->intent_slot_json = AllocateNestedCompanyString(
            spec.GetCapacity("intent_slot_json"), out_block);
        out_block->raw_struct = raw;
        return 0;
      },
      [](void* ptr, const ResolvedOutputPoolSpec& /*spec*/) noexcept {
        if (!ptr) return;
        auto* raw = static_cast<CompanyOperatorAudioOutput*>(ptr);
        raw->request_id = 0;
        raw->status_code = 0;
        ResetNestedCompanyString(raw->transcribed_text);
        ResetNestedCompanyString(raw->intent_slot_json);
      }));

  // 16. rerank_in -> CompanyOperatorRerankInput
  RegisterBinding(MakeInputBinding(
      "rerank_in", "CompanyOperatorRerankInput",
      [](const void* ptr, const ResolvedInputLimits& limits,
         std::string* err) -> int {
        if (!ptr) {
          if (err) *err = "CompanyOperatorRerankInput pointer is null";
          return -3;
        }
        const auto* in = static_cast<const CompanyOperatorRerankInput*>(ptr);
        if (!in->query_text) {
          if (err) *err = "query_text pointer is null";
          return -3;
        }
        int ret = ValidateCompanyString(in->query_text, limits.max_text_bytes,
                                        "query_text", err);
        if (ret != 0) return ret;
        if (in->candidate_count <= 0 ||
            in->candidate_count > COMPANY_OPERATOR_MAX_RERANK_CANDIDATES) {
          if (err) {
            *err = "candidate_count " + std::to_string(in->candidate_count) +
                   " out of valid range [1, " +
                   std::to_string(COMPANY_OPERATOR_MAX_RERANK_CANDIDATES) + "]";
          }
          return -3;
        }
        for (int i = 0; i < in->candidate_count; ++i) {
          if (!in->candidate_passages[i]) {
            if (err)
              *err = "candidate_passages[" + std::to_string(i) + "] is null";
            return -3;
          }
          ret = ValidateCompanyString(
              in->candidate_passages[i], limits.max_doc_text_bytes,
              ("candidate_passages[" + std::to_string(i) + "]").c_str(), err);
          if (ret != 0) return ret;
        }
        return 0;
      }));

  // 17. rerank_out -> CompanyOperatorRerankOutput
  RegisterBinding(MakeOutputBinding(
      "rerank_out", "CompanyOperatorRerankOutput", {}, 0,
      sizeof(CompanyOperatorRerankOutput),
      [](const ResolvedOutputPoolSpec& /*spec*/, OwnedExternalBlock* out_block,
         std::string* /*err*/) -> int {
        auto* raw =
            AllocateRootOutput<CompanyOperatorRerankOutput>(1, out_block);
        if (!raw) return -4;

        raw->request_id = 0;
        raw->count = 0;
        raw->status_code = 0;
        for (int i = 0; i < COMPANY_OPERATOR_MAX_RERANK_CANDIDATES; ++i) {
          raw->scores[i] = 0.0f;
          raw->sorted_indices[i] = -1;
        }
        out_block->raw_struct = raw;
        return 0;
      },
      [](void* ptr, const ResolvedOutputPoolSpec& /*spec*/) noexcept {
        if (!ptr) return;
        auto* raw = static_cast<CompanyOperatorRerankOutput*>(ptr);
        raw->request_id = 0;
        raw->count = 0;
        raw->status_code = 0;
        for (int i = 0; i < COMPANY_OPERATOR_MAX_RERANK_CANDIDATES; ++i) {
          raw->scores[i] = 0.0f;
          raw->sorted_indices[i] = -1;
        }
      }));
}

}  // namespace llm_edgeflow

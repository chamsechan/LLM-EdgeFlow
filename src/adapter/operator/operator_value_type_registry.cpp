#include "adapter/operator/operator_value_type_registry.h"

#include <stdexcept>

#include "contracts/diagnostic.h"
#include "operator/company_operator_types.h"

namespace llm_edgeflow {

namespace {

const CompanyAnyTypeDescriptor kBuiltinAnyTypes[] = {
    {0, 0, 1, "none"},
    {1, sizeof(float), alignof(float), "float32"},
    {2, sizeof(int32_t), alignof(int32_t), "int32"},
    {3, sizeof(uint8_t), alignof(uint8_t), "uint8"},
    {4, sizeof(int64_t), alignof(int64_t), "int64"},
    {5, sizeof(double), alignof(double), "float64"},
};

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
    SetDiagnosticNoexcept(err, e.what());
    return false;
  } catch (...) {
    SetDiagnosticNoexcept(err, "Unknown exception resolving output pool spec");
    return false;
  }
}

bool ComputeOutputPoolPayloadBytes(const OperatorValueTypeBinding& binding,
                                   const ResolvedOutputPoolSpec& spec,
                                   uint32_t depth, size_t* out_bytes,
                                   std::string* err) noexcept {
  try {
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
      SetDiagnosticNoexcept(err, e.what());
      return false;
    } catch (...) {
      SetDiagnosticNoexcept(err,
                            "Unknown exception computing output block payload");
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
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(err, e.what());
    return false;
  } catch (...) {
    SetDiagnosticNoexcept(
        err, "Unknown exception computing output pool payload bytes");
    return false;
  }
}

bool ComputeOutputPoolPayloadBytes(const std::string& suffix,
                                   const ResolvedOutputPoolSpec& spec,
                                   uint32_t depth, size_t* out_bytes,
                                   std::string* err) noexcept {
  try {
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
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(err, e.what());
    return false;
  } catch (...) {
    SetDiagnosticNoexcept(err,
                          "Unknown exception in ComputeOutputPoolPayloadBytes");
    return false;
  }
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
  try {
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
  } catch (...) {
    return false;
  }
}

int OperatorValueTypeRegistry::ValidateCompanyString(
    const CompanyString* str, size_t max_bytes, const char* field_name,
    std::string* err) noexcept {
  try {
    const char* name = field_name ? field_name : "string";
    if (!str) {
      if (err) *err = std::string(name) + " pointer is null";
      return -3;
    }
    if (str->length < 0) {
      if (err)
        *err = std::string(name) + " has negative length " +
               std::to_string(str->length);
      return -3;
    }
    if (static_cast<size_t>(str->length) > max_bytes) {
      if (err)
        *err = std::string(name) + " length " + std::to_string(str->length) +
               " exceeds max limit " + std::to_string(max_bytes);
      return -3;
    }
    if (str->length > 0) {
      if (!str->data) {
        if (err)
          *err = std::string(name) + " length is " +
                 std::to_string(str->length) + " but data pointer is null";
        return -3;
      }
      for (int32_t i = 0; i < str->length; ++i) {
        if (str->data[i] == '\0') {
          if (err)
            *err = std::string(name) +
                   " contains forbidden embedded NUL at byte offset " +
                   std::to_string(i);
          return -3;
        }
      }
    }
    return 0;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(err, e.what());
    return -3;
  } catch (...) {
    SetDiagnosticNoexcept(err, "Unknown exception in ValidateCompanyString");
    return -3;
  }
}

int OperatorValueTypeRegistry::ValidateCompanyBuffer(
    const CompanyBuffer* buf, size_t max_bytes, const char* field_name,
    std::string* err) noexcept {
  try {
    const char* name = field_name ? field_name : "buffer";
    if (!buf) {
      if (err) *err = std::string(name) + " pointer is null";
      return -3;
    }
    if (buf->length < 0) {
      if (err)
        *err = std::string(name) + " has negative length " +
               std::to_string(buf->length);
      return -3;
    }
    if (static_cast<size_t>(buf->length) > max_bytes) {
      if (err)
        *err = std::string(name) + " length exceeds max limit " +
               std::to_string(max_bytes);
      return -3;
    }
    if (buf->length > 0 && !buf->data) {
      if (err) *err = std::string(name) + " data pointer is null";
      return -3;
    }
    return 0;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(err, e.what());
    return -3;
  } catch (...) {
    SetDiagnosticNoexcept(err, "Unknown exception in ValidateCompanyBuffer");
    return -3;
  }
}

int OperatorValueTypeRegistry::ValidateCompanyAnyPayload(
    const CompanyAny* any, size_t max_any_bytes, const char* field_name,
    std::string* err) noexcept {
  try {
    const char* name = field_name ? field_name : "any";
    if (!any) {
      if (err) *err = std::string(name) + " pointer is null";
      return -3;
    }
    if (any->element_count < 0 || any->byte_length < 0) {
      if (err) {
        *err = std::string(name) + " has negative count or length: count=" +
               std::to_string(any->element_count) +
               ", length=" + std::to_string(any->byte_length);
      }
      return -3;
    }
    if (static_cast<size_t>(any->byte_length) > max_any_bytes) {
      if (err) {
        *err = std::string(name) + " byte_length " +
               std::to_string(any->byte_length) + " exceeds max limit " +
               std::to_string(max_any_bytes);
      }
      return -3;
    }
    if (any->type_id == 0) {
      if (any->element_count != 0 || any->byte_length != 0) {
        if (err) {
          *err = std::string(name) + " has type_id=0 but non-zero count/length";
        }
        return -3;
      }
      return 0;
    }

    const auto* desc = FindCompanyAnyType(any->type_id);
    if (!desc) {
      if (err) {
        *err = std::string(name) + " has unknown or unwhitelisted type_id " +
               std::to_string(any->type_id);
      }
      return -3;
    }

    size_t expected_bytes = 0;
    if (!CheckedMultiply(static_cast<size_t>(any->element_count),
                         desc->element_size, &expected_bytes)) {
      if (err) {
        *err = std::string(name) + " element_count multiplication overflowed";
      }
      return -3;
    }
    if (expected_bytes != static_cast<size_t>(any->byte_length)) {
      if (err) {
        *err = std::string(name) + " size equation mismatch: expected " +
               std::to_string(expected_bytes) + " bytes for " +
               std::to_string(any->element_count) + " elements of type " +
               desc->debug_name + ", but byte_length is " +
               std::to_string(any->byte_length);
      }
      return -3;
    }
    if (any->byte_length > 0 && !any->data) {
      if (err) {
        *err = std::string(name) + " non-empty payload has null data";
      }
      return -3;
    }
    return 0;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(err, e.what());
    return -3;
  } catch (...) {
    SetDiagnosticNoexcept(err,
                          "Unknown exception in ValidateCompanyAnyPayload");
    return -3;
  }
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
    auto temp_bindings = bindings_by_canonical_;

    temp_bindings[binding.canonical_suffix] = binding;

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

}  // namespace llm_edgeflow

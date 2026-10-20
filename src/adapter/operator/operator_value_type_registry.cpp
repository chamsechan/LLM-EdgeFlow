#include "adapter/operator/operator_value_type_registry.h"

#include <stdexcept>

#include "contracts/diagnostic.h"
#include "contracts/error_codes.h"

namespace llm_edgeflow {

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

    if (requested.allocator != binding.allocation_name) {
      if (err) *err = "Output allocator does not match selected binding";
      return false;
    }
    if (static_cast<bool>(binding.normalize_parameters) !=
        static_cast<bool>(requested.params)) {
      if (err)
        *err = binding.normalize_parameters
                   ? "Output allocator requires normalized parameters"
                   : "Selected output allocator does not accept params";
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
        if (err) *err = "Missing output capacity field: " + field;
        return false;
      }
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
    const auto& validate_metadata = binding.output_layout.validate_metadata;
    if (validate_metadata
            ? !validate_metadata(candidate.meta_num, candidate.metadata_type_id)
            : candidate.meta_num != 0 || candidate.metadata_type_id != 0) {
      if (err) *err = "Invalid metadata count or type for platform binding";
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

OperatorValueTypeRegistry& OperatorValueTypeRegistry::Instance() {
  static OperatorValueTypeRegistry instance;
  return instance;
}

bool OperatorValueTypeRegistry::ParseKey(const std::string& key,
                                         std::string* out_namespace,
                                         std::string* out_suffix) noexcept {
  try {
    if (key.empty()) return false;
    // 宿主路由 key 固定为 <prefix>.<type>，两段均非空且只有一个 '.'。
    const size_t last_dot = key.find('.');
    if (last_dot == std::string::npos || last_dot == 0 ||
        last_dot == key.size() - 1 ||
        key.find('.', last_dot + 1) != std::string::npos) {
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

bool OperatorValueTypeRegistry::HasConflict() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_conflict_;
}

int OperatorValueTypeRegistry::GlobalInit() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (has_conflict_) {
    return error::kRegistryConflict;
  }
  if (audited_) {
    return error::kOk;
  }
  const auto valid_binding = [](const OperatorValueTypeBinding& binding) {
    if (binding.canonical_suffix.empty() ||
        binding.external_c_type_name.empty()) {
      return false;
    }
    if (binding.direction == IoDirection::kOutput) {
      if (!binding.allocate_external || !binding.reset_external ||
          !binding.destroy_external ||
          !binding.output_layout.compute_block_payload_bytes) {
        return false;
      }
      for (const auto& [field, config] :
           binding.output_layout.string_capacity_fields) {
        if (field.empty() || config.max_capacity == 0) {
          return false;
        }
      }
    } else if (binding.direction == IoDirection::kInput) {
      if (!binding.validate_external) {
        return false;
      }
    } else {
      return false;
    }
    return true;
  };
  for (const auto& [suffix, binding] : bindings_by_canonical_) {
    if (!valid_binding(binding)) {
      has_conflict_ = true;
      return error::kRegistryConflict;
    }
  }
  for (const auto& [name, binding] : output_allocators_) {
    const auto root = bindings_by_canonical_.find(binding.canonical_suffix);
    if (!valid_binding(binding) || root == bindings_by_canonical_.end() ||
        root->second.direction != IoDirection::kOutput ||
        root->second.external_c_type_name != binding.external_c_type_name) {
      has_conflict_ = true;
      return error::kRegistryConflict;
    }
  }
  audited_ = true;
  return error::kOk;
}

bool OperatorValueTypeRegistry::RegisterBinding(
    const OperatorValueTypeBinding& binding) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (audited_) {
    return false;
  }
  if (binding.canonical_suffix.empty() ||
      binding.external_c_type_name.empty() ||
      !binding.allocation_name.empty()) {
    has_conflict_ = true;
    return false;
  }

  // 检查 canonical 是否与已有 canonical 冲突
  if (bindings_by_canonical_.find(binding.canonical_suffix) !=
      bindings_by_canonical_.end()) {
    has_conflict_ = true;
    return false;
  }

  try {
    auto candidate = binding;
    bindings_by_canonical_.emplace(binding.canonical_suffix,
                                   std::move(candidate));
  } catch (...) {
    // 资源分配或复制异常不污染契约冲突状态，已有记录保持不变。
    return false;
  }
  return true;
}

bool OperatorValueTypeRegistry::RegisterOutputAllocator(
    const std::string& name, const OperatorValueTypeBinding& binding) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (audited_) return false;
  if (name.empty() || binding.canonical_suffix.empty() ||
      binding.external_c_type_name.empty() ||
      binding.direction != IoDirection::kOutput ||
      output_allocators_.count(name)) {
    has_conflict_ = true;
    return false;
  }
  try {
    auto candidate = binding;
    candidate.allocation_name = name;
    output_allocators_.emplace(name, std::move(candidate));
  } catch (...) {
    return false;
  }
  return true;
}

const OperatorValueTypeBinding* OperatorValueTypeRegistry::GetOutputBinding(
    const std::string& suffix, const std::string& allocator) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto& entries =
      allocator.empty() ? bindings_by_canonical_ : output_allocators_;
  const auto it = entries.find(allocator.empty() ? suffix : allocator);
  if (it == entries.end() || it->second.canonical_suffix != suffix ||
      it->second.direction != IoDirection::kOutput)
    return nullptr;
  return &it->second;
}

bool NormalizeOutputParameters(
    const OperatorValueTypeBinding& binding, const std::string& requested,
    std::shared_ptr<const OutputAllocationParameters>* normalized,
    std::string* error) noexcept {
  try {
    if (!normalized) {
      if (error) *error = "Null normalized output parameters destination";
      return false;
    }
    normalized->reset();
    if (!binding.normalize_parameters) {
      if (!requested.empty() && requested != "{}") {
        if (error) *error = "Selected output allocator does not accept params";
        return false;
      }
      return true;
    }
    std::shared_ptr<const OutputAllocationParameters> candidate;
    if (!binding.normalize_parameters(requested, &candidate, error))
      return false;
    if (!candidate) {
      if (error)
        *error = "Output allocator did not produce normalized parameters";
      return false;
    }
    *normalized = std::move(candidate);
    return true;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(error, e.what());
  } catch (...) {
    SetDiagnosticNoexcept(error,
                          "Unknown exception normalizing output parameters");
  }
  return false;
}

bool RegisterOperatorValueType(const OperatorValueTypeBinding& binding) {
  return OperatorValueTypeRegistry::Instance().RegisterBinding(binding);
}

bool RegisterOperatorOutputAllocator(const std::string& name,
                                     const OperatorValueTypeBinding& binding) {
  return OperatorValueTypeRegistry::Instance().RegisterOutputAllocator(name,
                                                                       binding);
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

std::optional<OperatorValueTypeBinding>
OperatorValueTypeRegistry::CopyBindingBySuffix(
    const std::string& suffix) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = bindings_by_canonical_.find(suffix);
  if (it == bindings_by_canonical_.end()) return std::nullopt;
  return it->second;
}

std::optional<OperatorValueTypeBinding>
OperatorValueTypeRegistry::CopyOutputBinding(
    const std::string& suffix, const std::string& allocator) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto& entries =
      allocator.empty() ? bindings_by_canonical_ : output_allocators_;
  const auto it = entries.find(allocator.empty() ? suffix : allocator);
  if (it == entries.end() || it->second.canonical_suffix != suffix ||
      it->second.direction != IoDirection::kOutput)
    return std::nullopt;
  return it->second;
}

}  // namespace llm_edgeflow

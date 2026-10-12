#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "adapter/io_converter_registry.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "core/node_registry.h"

namespace llm_edgeflow::test_support {

// Restore native singleton state. Swapped callback owners are destroyed only
// after the destructor's lock has been released; they may reenter the registry.
class RegistryTestAccess {
 public:
  class ScopedNodeState {
   public:
    ScopedNodeState() {
      auto& registry = NodeRegistry::Instance();
      std::lock_guard<std::mutex> lock(registry.mutex_);
      entries_ = registry.entries_;
      errors_ = registry.conflict_errors_;
      conflict_ = registry.has_conflict_.load();
    }
    ~ScopedNodeState() {
      auto& registry = NodeRegistry::Instance();
      std::lock_guard<std::mutex> lock(registry.mutex_);
      registry.entries_.swap(entries_);
      registry.conflict_errors_.swap(errors_);
      registry.has_conflict_.store(conflict_);
    }
    ScopedNodeState(const ScopedNodeState&) = delete;
    ScopedNodeState& operator=(const ScopedNodeState&) = delete;

   private:
    std::unordered_map<std::string, NodeRegistry::EntryHandle> entries_;
    std::vector<std::string> errors_;
    bool conflict_ = false;
  };

  static void ResetNodes() {
    std::unordered_map<std::string, NodeRegistry::EntryHandle> retired;
    auto& registry = NodeRegistry::Instance();
    {
      std::lock_guard<std::mutex> lock(registry.mutex_);
      retired.swap(registry.entries_);
      registry.conflict_errors_.clear();
      registry.has_conflict_.store(false);
    }
  }

  static void ResetConverters() {
    auto& registry = IoConverterRegistry::Instance();
    decltype(registry.input_converters_) inputs;
    decltype(registry.output_converters_) outputs;
    decltype(registry.output_parameters_) parameters;
    {
      std::lock_guard<std::mutex> audit_lock(registry.audit_mutex_);
      std::lock_guard<std::mutex> lock(registry.mutex_);
      inputs.swap(registry.input_converters_);
      outputs.swap(registry.output_converters_);
      parameters.swap(registry.output_parameters_);
      registry.conflicts_.Clear();
    }
  }

  class ScopedValueTypeState {
   public:
    ScopedValueTypeState() {
      auto& registry = OperatorValueTypeRegistry::Instance();
      std::lock_guard<std::mutex> lock(registry.mutex_);
      bindings_ = registry.bindings_by_canonical_;
      allocators_ = registry.output_allocators_;
      conflict_ = registry.has_conflict_;
      audited_ = registry.audited_;
      registry.audited_ = false;
    }
    ~ScopedValueTypeState() {
      auto& registry = OperatorValueTypeRegistry::Instance();
      std::lock_guard<std::mutex> lock(registry.mutex_);
      registry.bindings_by_canonical_.swap(bindings_);
      registry.output_allocators_.swap(allocators_);
      registry.has_conflict_ = conflict_;
      registry.audited_ = audited_;
    }
    ScopedValueTypeState(const ScopedValueTypeState&) = delete;
    ScopedValueTypeState& operator=(const ScopedValueTypeState&) = delete;

   private:
    std::unordered_map<std::string, OperatorValueTypeBinding> bindings_;
    std::unordered_map<std::string, OperatorValueTypeBinding> allocators_;
    bool conflict_ = false;
    bool audited_ = false;
  };

  static void SetValueBinding(OperatorValueTypeBinding binding) {
    auto& registry = OperatorValueTypeRegistry::Instance();
    std::lock_guard<std::mutex> lock(registry.mutex_);
    registry.bindings_by_canonical_[binding.canonical_suffix] =
        std::move(binding);
    registry.audited_ = false;
  }
};
}  // namespace llm_edgeflow::test_support

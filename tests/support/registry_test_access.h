#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter/operator/operator_value_type_registry.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow::test_support {

class RegistryTestAccess {
 public:
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
    std::unordered_map<std::string, OperatorValueTypeBinding> bindings_,
        allocators_;
    bool conflict_ = false, audited_ = false;
  };
  static void SetValueBinding(OperatorValueTypeBinding binding) {
    auto& registry = OperatorValueTypeRegistry::Instance();
    std::lock_guard<std::mutex> lock(registry.mutex_);
    registry.bindings_by_canonical_[binding.canonical_suffix] =
        std::move(binding);
    registry.audited_ = false;
  }
  static void SetService(const std::string& suffix, const std::string& name,
                         int32_t value) {
    auto& registry = OperatorValueTypeRegistry::Instance();
    std::lock_guard<std::mutex> lock(registry.mutex_);
    registry.bindings_by_canonical_.at(suffix).services[name] = value;
    registry.audited_ = false;
  }
  static void ResetNodes() {
    std::unordered_map<std::string, NodeRegistry::EntryHandle> old_entries;
    {
      std::lock_guard<std::mutex> lock(NodeRegistry::Instance().mutex_);
      old_entries.swap(NodeRegistry::Instance().entries_);
      NodeRegistry::Instance().has_conflict_.store(false,
                                                   std::memory_order_release);
      NodeRegistry::Instance().conflict_errors_.clear();
    }
    // old_entries 在锁外析构
  }

  static void ClearNodeFailures() noexcept {
    try {
      std::lock_guard<std::mutex> lock(NodeRegistry::Instance().mutex_);
      NodeRegistry::Instance().has_conflict_.store(false,
                                                   std::memory_order_release);
      NodeRegistry::Instance().conflict_errors_.clear();
    } catch (...) {
    }
  }

  class ScopedNodeState {
   public:
    ScopedNodeState() {
      std::lock_guard<std::mutex> lock(NodeRegistry::Instance().mutex_);
      saved_entries_ = NodeRegistry::Instance().entries_;
      saved_has_conflict_ = NodeRegistry::Instance().has_conflict_.load(
          std::memory_order_acquire);
      saved_conflict_errors_ = NodeRegistry::Instance().conflict_errors_;
    }

    ~ScopedNodeState() noexcept {
      std::unordered_map<std::string, NodeRegistry::EntryHandle> old_entries;
      std::vector<std::string> old_errors;
      try {
        std::lock_guard<std::mutex> lock(NodeRegistry::Instance().mutex_);
        old_entries.swap(NodeRegistry::Instance().entries_);
        NodeRegistry::Instance().entries_.swap(saved_entries_);
        NodeRegistry::Instance().has_conflict_.store(saved_has_conflict_,
                                                     std::memory_order_release);
        old_errors.swap(NodeRegistry::Instance().conflict_errors_);
        NodeRegistry::Instance().conflict_errors_.swap(saved_conflict_errors_);
      } catch (...) {
      }
      // old_entries、old_errors 在锁外析构，且不分配内存
    }

    ScopedNodeState(const ScopedNodeState&) = delete;
    ScopedNodeState& operator=(const ScopedNodeState&) = delete;

   private:
    std::unordered_map<std::string, NodeRegistry::EntryHandle> saved_entries_;
    bool saved_has_conflict_ = false;
    std::vector<std::string> saved_conflict_errors_;
  };
};

}  // namespace llm_edgeflow::test_support

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/node_registry.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow::test_support {

class RegistryTestAccess {
 public:
  static void ResetNodes() {
    std::unordered_map<std::string, NodeRegistry::EntryHandle> old_entries;
    {
      std::lock_guard<std::mutex> lock(NodeRegistry::Instance().mutex_);
      old_entries.swap(NodeRegistry::Instance().entries_);
      NodeRegistry::Instance().has_conflict_.store(false,
                                                   std::memory_order_release);
      NodeRegistry::Instance().conflict_errors_.clear();
    }
    // old_entries are destructed outside the lock
  }

  static void ResetBizs() { PipelineCatalog::ResetBizsForTesting(); }

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
      try {
        std::unordered_map<std::string, NodeRegistry::EntryHandle>
            restore_entries = saved_entries_;
        std::vector<std::string> restore_errors = saved_conflict_errors_;
        std::unordered_map<std::string, NodeRegistry::EntryHandle> old_entries;
        {
          std::lock_guard<std::mutex> lock(NodeRegistry::Instance().mutex_);
          old_entries.swap(NodeRegistry::Instance().entries_);
          NodeRegistry::Instance().entries_.swap(restore_entries);
          NodeRegistry::Instance().has_conflict_.store(
              saved_has_conflict_, std::memory_order_release);
          NodeRegistry::Instance().conflict_errors_.swap(restore_errors);
        }
        // old_entries, restore_entries, restore_errors destructed outside lock
      } catch (...) {
      }
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

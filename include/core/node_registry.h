#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "company_alg_log.h"
#include "core/node_base.h"
#include "core/pipeline_catalog.h"

namespace alg_framework {

class NodeFactory {
 public:
  using CreatorFunc = std::function<std::unique_ptr<INode>()>;

  static NodeFactory& Instance() {
    static NodeFactory instance;
    return instance;
  }

  bool Register(const std::string& node_type, CreatorFunc creator,
                const NodeDefinition* definition) noexcept {
    try {
      if (definition == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        has_conflict_ = true;
        conflict_errors_.push_back(
            "Node registration requires a valid Definition: " + node_type);
        return false;
      }
      if (definition->node_type != node_type) {
        std::lock_guard<std::mutex> lock(mutex_);
        has_conflict_ = true;
        conflict_errors_.push_back(
            "NodeDefinition node_type mismatch: expected " + node_type +
            ", got " + definition->node_type);
        return false;
      }
      if (node_type.empty() || !creator) {
        std::lock_guard<std::mutex> lock(mutex_);
        has_conflict_ = true;
        conflict_errors_.push_back("Empty node_type or null creator function");
        return false;
      }
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = creators_.find(node_type);
      if (it != creators_.end()) {
        has_conflict_ = true;
        conflict_errors_.push_back("Duplicate node registration for type: " +
                                   node_type);
        ALG_LOG_ERROR("[NodeFactory] Duplicate node registration: %s\n",
                      node_type.c_str());
        return false;
      }
      if (!PipelineCatalog::RegisterNodeDefinition(*definition)) {
        has_conflict_ = true;
        conflict_errors_.push_back("Invalid or duplicate node Definition: " +
                                   node_type);
        return false;
      }
      creators_[node_type] = std::move(creator);
      return true;
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lock(mutex_);
      has_conflict_ = true;
      conflict_errors_.push_back("Exception registering node " + node_type +
                                 ": " + e.what());
      return false;
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      has_conflict_ = true;
      conflict_errors_.push_back("Unknown exception registering node " +
                                 node_type);
      return false;
    }
  }

  bool Register(const std::string& node_type, CreatorFunc creator,
                const NodeDefinition& definition) noexcept {
    return Register(node_type, std::move(creator), &definition);
  }

  std::unique_ptr<INode> Create(const std::string& node_type) const {
    CreatorFunc creator;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = creators_.find(node_type);
      if (it == creators_.end()) return nullptr;
      creator = it->second;
    }
    // R1-ACC-004: 锁外执行外部 creator，避免嵌套查询或构造导致自锁
    return creator();
  }

  bool Has(const std::string& node_type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return creators_.find(node_type) != creators_.end();
  }

  std::vector<std::string> ListTypes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    result.reserve(creators_.size());
    for (const auto& item : creators_) result.push_back(item.first);
    std::sort(result.begin(), result.end());
    return result;
  }

  bool HasConflict() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return has_conflict_;
  }

  std::vector<std::string> GetConflictErrors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return conflict_errors_;
  }

  void ClearForTesting() {
    std::lock_guard<std::mutex> lock(mutex_);
    creators_.clear();
    has_conflict_ = false;
    conflict_errors_.clear();
  }

 private:
  NodeFactory() = default;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, CreatorFunc> creators_;
  bool has_conflict_ = false;
  std::vector<std::string> conflict_errors_;
};

#define REGISTER_NODE_WITH_DEFINITION(NodeType, ...)                        \
  static bool _reg_node_##NodeType = []() noexcept {                        \
    const auto definition = (__VA_ARGS__);                                  \
    return ::alg_framework::NodeFactory::Instance().Register(               \
        NodeType::kNodeType, []() { return std::make_unique<NodeType>(); }, \
        &definition);                                                       \
  }()

}  // namespace alg_framework

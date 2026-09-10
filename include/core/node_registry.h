#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/node_definition.h"
#include "core/node_interface.h"
#include "edgeflow/log.h"

namespace llm_edgeflow {

class NodeRegistry {
 public:
  using CreatorFunc = std::function<std::unique_ptr<INode>()>;

  static NodeRegistry& Instance() {
    static NodeRegistry instance;
    return instance;
  }

  bool Register(const std::string& node_type, CreatorFunc creator,
                const NodeDefinition* definition) noexcept;

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
  void RecordRegistrationFailure(std::string_view message) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      has_conflict_ = true;
      conflict_errors_.emplace_back(message);
    } catch (...) {
      // Registration remains failed even if its diagnostic cannot be stored.
    }
  }

  NodeRegistry() = default;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, CreatorFunc> creators_;
  bool has_conflict_ = false;
  std::vector<std::string> conflict_errors_;
};

// Source compatibility for extensions using the former name.
using NodeFactory = NodeRegistry;

#define REGISTER_NODE_WITH_DEFINITION(NodeType, ...)                        \
  static bool _reg_node_##NodeType = []() noexcept {                        \
    const auto definition = (__VA_ARGS__);                                  \
    return ::llm_edgeflow::NodeRegistry::Instance().Register(               \
        NodeType::kNodeType, []() { return std::make_unique<NodeType>(); }, \
        &definition);                                                       \
  }()

}  // namespace llm_edgeflow

#pragma once

#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/node_base.h"

namespace alg_framework {

class NodeFactory {
 public:
  using CreatorFunc = std::function<std::unique_ptr<INode>()>;

  static NodeFactory& Instance() {
    static NodeFactory instance;
    return instance;
  }

  bool Register(const std::string& node_type, CreatorFunc creator) noexcept {
    try {
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
        std::cerr << "[NodeFactory ERROR] Duplicate node registration: "
                  << node_type << std::endl;
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

  bool HasConflict() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return has_conflict_;
  }

  std::vector<std::string> GetConflictErrors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return conflict_errors_;
  }

 private:
  NodeFactory() = default;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, CreatorFunc> creators_;
  bool has_conflict_ = false;
  std::vector<std::string> conflict_errors_;
};

#define REGISTER_NODE(NodeType)                                      \
  static bool _registered_node_##NodeType = []() noexcept {          \
    return ::alg_framework::NodeFactory::Instance().Register(        \
        #NodeType, []() -> std::unique_ptr<::alg_framework::INode> { \
          return std::make_unique<NodeType>();                       \
        });                                                          \
  }()

}  // namespace alg_framework

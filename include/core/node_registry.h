#pragma once

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/node_definition.h"
#include "core/node_interface.h"
#include "edgeflow/log.h"

namespace llm_edgeflow {

namespace test_support {
class RegistryTestAccess;
}

struct NodeRegistrySnapshot {
  std::vector<NodeDefinition> definitions;  // node_type 升序，独立值副本
  bool has_conflict = false;
  std::vector<std::string> conflict_errors;
};

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

  template <typename CreatorCallable, typename FactoryCallable>
  bool RegisterWithDefinitionFactory(std::string_view node_type,
                                     CreatorCallable&& creator,
                                     FactoryCallable&& factory) noexcept {
    try {
      NodeDefinition definition = factory();
      CreatorFunc creator_fn = [c = std::forward<CreatorCallable>(
                                    creator)]() mutable { return c(); };
      return Register(std::string(node_type), std::move(creator_fn),
                      &definition);
    } catch (const std::exception& e) {
      RecordRegistrationFailure(node_type, " definition error: ", e.what());
      return false;
    } catch (...) {
      RecordRegistrationFailure(node_type,
                                " definition error: ", "unknown exception");
      return false;
    }
  }

  std::unique_ptr<INode> Create(const std::string& node_type) const;
  bool Has(const std::string& node_type) const;
  std::vector<std::string> ListTypes() const;
  bool HasConflict() const;
  std::vector<std::string> GetConflictErrors() const;

  NodeRegistrySnapshot Snapshot() const;
  std::optional<NodeDefinition> Find(const std::string& node_type) const;
  std::vector<NodeDefinition> ListDefinitions() const;

 private:
  friend class test_support::RegistryTestAccess;

  struct Entry {
    NodeDefinition definition;
    CreatorFunc creator;
  };
  using EntryHandle = std::shared_ptr<const Entry>;

  static bool CheckCrossNodeControlConflict(
      const NodeDefinition& definition,
      const std::unordered_map<std::string, EntryHandle>& entries,
      std::string* error);

  void RecordRegistrationFailure(std::string_view message) noexcept;
  void RecordRegistrationFailure(std::string_view prefix,
                                 std::string_view separator,
                                 std::string_view message) noexcept;

  NodeRegistry() = default;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, EntryHandle> entries_;
  std::atomic<bool> has_conflict_{false};
  std::vector<std::string> conflict_errors_;
};

#define REGISTER_NODE_WITH_DEFINITION(NodeType, ...)       \
  static bool _reg_node_##NodeType = []() noexcept {       \
    return ::llm_edgeflow::NodeRegistry::Instance()        \
        .RegisterWithDefinitionFactory(                    \
            NodeType::kNodeType,                           \
            []() { return std::make_unique<NodeType>(); }, \
            []() { return (__VA_ARGS__); });               \
  }()

}  // namespace llm_edgeflow

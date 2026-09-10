#include "core/node_registry.h"

#include "core/pipeline_catalog.h"

namespace llm_edgeflow {

bool NodeRegistry::Register(const std::string& node_type, CreatorFunc creator,
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
      ALG_LOG_ERROR("[NodeRegistry] Duplicate node registration: %s\n",
                    node_type.c_str());
      return false;
    }
    std::string definition_error;
    if (!PipelineCatalog::RegisterNodeDefinition(*definition,
                                                 &definition_error)) {
      has_conflict_ = true;
      ALG_LOG_ERROR("[NodeRegistry] %s\n", definition_error.c_str());
      conflict_errors_.push_back(std::move(definition_error));
      return false;
    }
    creators_[node_type] = std::move(creator);
    return true;
  } catch (const std::exception& e) {
    RecordRegistrationFailure(e.what());
    return false;
  } catch (...) {
    RecordRegistrationFailure("Unknown exception registering node");
    return false;
  }
}

}  // namespace llm_edgeflow

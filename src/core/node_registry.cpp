#include "core/node_registry.h"

#include <algorithm>
#include <utility>

#include "core/node_definition_validation.h"

namespace llm_edgeflow {

bool NodeRegistry::CheckCrossNodeControlConflict(
    const NodeDefinition& definition,
    const std::unordered_map<std::string, EntryHandle>& entries,
    std::string* error) {
  std::string best_conflict_node;
  int best_conflict_cmd_id = 0;
  bool found_conflict = false;

  for (const auto& [existing_type, entry] : entries) {
    if (!entry) continue;
    const auto& existing_def = entry->definition;
    for (const auto& command : definition.control_commands) {
      for (const auto& registered : existing_def.control_commands) {
        if (command.cmd_id != registered.cmd_id) continue;
        if (!command.shared_id || !registered.shared_id ||
            command.name != registered.name ||
            command.payload_schema != registered.payload_schema ||
            command.supports_hot_swap != registered.supports_hot_swap) {
          if (!found_conflict || existing_def.node_type < best_conflict_node) {
            found_conflict = true;
            best_conflict_node = existing_def.node_type;
            best_conflict_cmd_id = command.cmd_id;
          }
        }
      }
    }
  }

  if (found_conflict) {
    if (error) {
      *error = "Control ID " + std::to_string(best_conflict_cmd_id) +
               " conflicts between " + best_conflict_node + " and " +
               definition.node_type +
               "; shared commands require shared_id and identical contracts";
    }
    return false;
  }
  return true;
}

bool NodeRegistry::Register(const std::string& node_type, CreatorFunc creator,
                            const NodeDefinition* definition) noexcept {
  EntryHandle candidate;
  std::string failure_msg;
  try {
    if (definition == nullptr) {
      failure_msg =
          "Node registration requires a valid Definition: " + node_type;
    } else if (definition->node_type != node_type) {
      failure_msg = "NodeDefinition node_type mismatch: expected " + node_type +
                    ", got " + definition->node_type;
    } else if (node_type.empty() || !creator) {
      failure_msg = "Empty node_type or null creator function";
    } else {
      std::string schema_error;
      if (!ValidateNodeDefinitionStructure(*definition, &schema_error)) {
        failure_msg = schema_error.empty()
                          ? ("Invalid NodeDefinition: " + node_type)
                          : schema_error;
      } else {
        candidate = std::make_shared<const Entry>(
            Entry{*definition, std::move(creator)});
      }
    }
  } catch (const std::exception& e) {
    failure_msg = e.what();
  } catch (...) {
    failure_msg = "Unknown exception during node preparation";
  }

  if (!candidate) {
    if (!failure_msg.empty()) {
      ALG_LOG_ERROR("[NodeRegistry] %s\n", failure_msg.c_str());
    }
    RecordRegistrationFailure(failure_msg);
    return false;
  }

  bool inserted = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(node_type);
    if (it != entries_.end()) {
      failure_msg = "Duplicate node registration for type: " + node_type;
    } else {
      std::string control_error;
      if (!CheckCrossNodeControlConflict(candidate->definition, entries_,
                                         &control_error)) {
        failure_msg = std::move(control_error);
      } else {
        try {
          auto res = entries_.try_emplace(node_type, candidate);
          inserted = res.second;
        } catch (const std::exception& e) {
          failure_msg = e.what();
        } catch (...) {
          failure_msg = "Unknown exception during entry insertion";
        }
      }
    }
  }

  if (!inserted) {
    if (!failure_msg.empty()) {
      ALG_LOG_ERROR("[NodeRegistry] %s\n", failure_msg.c_str());
    }
    RecordRegistrationFailure(failure_msg);
    return false;
  }

  return true;
}

std::unique_ptr<INode> NodeRegistry::Create(
    const std::string& node_type) const {
  EntryHandle handle;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(node_type);
    if (it == entries_.end() || !it->second) return nullptr;
    handle = it->second;
  }
  CreatorFunc creator = handle->creator;
  if (!creator) return nullptr;
  return creator();
}

bool NodeRegistry::Has(const std::string& node_type) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.find(node_type) != entries_.end();
}

std::vector<std::string> NodeRegistry::ListTypes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> result;
  result.reserve(entries_.size());
  for (const auto& item : entries_) result.push_back(item.first);
  std::sort(result.begin(), result.end());
  return result;
}

bool NodeRegistry::HasConflict() const {
  return has_conflict_.load(std::memory_order_acquire);
}

std::vector<std::string> NodeRegistry::GetConflictErrors() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return conflict_errors_;
}

NodeRegistrySnapshot NodeRegistry::Snapshot() const {
  std::vector<EntryHandle> handles;
  bool conflict = false;
  std::vector<std::string> errors;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    handles.reserve(entries_.size());
    for (const auto& pair : entries_) {
      handles.push_back(pair.second);
    }
    conflict = has_conflict_.load(std::memory_order_acquire);
    errors = conflict_errors_;
  }

  std::vector<NodeDefinition> defs;
  defs.reserve(handles.size());
  for (const auto& handle : handles) {
    if (handle) {
      defs.push_back(handle->definition);
    }
  }
  std::sort(defs.begin(), defs.end(), [](const auto& a, const auto& b) {
    return a.node_type < b.node_type;
  });
  return {std::move(defs), conflict, std::move(errors)};
}

std::optional<NodeDefinition> NodeRegistry::Find(
    const std::string& node_type) const {
  EntryHandle handle;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(node_type);
    if (it == entries_.end() || !it->second) return std::nullopt;
    handle = it->second;
  }
  return handle->definition;
}

std::vector<NodeDefinition> NodeRegistry::ListDefinitions() const {
  return Snapshot().definitions;
}

void NodeRegistry::RecordRegistrationFailure(
    std::string_view message) noexcept {
  has_conflict_.store(true, std::memory_order_release);
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    conflict_errors_.emplace_back(message);
  } catch (...) {
  }
}

void NodeRegistry::RecordRegistrationFailure(
    std::string_view prefix, std::string_view separator,
    std::string_view message) noexcept {
  has_conflict_.store(true, std::memory_order_release);
  try {
    std::string full;
    full.reserve(prefix.size() + separator.size() + message.size());
    full.append(prefix);
    full.append(separator);
    full.append(message);
    std::lock_guard<std::mutex> lock(mutex_);
    conflict_errors_.push_back(std::move(full));
  } catch (...) {
  }
}

}  // namespace llm_edgeflow

#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "core/node_definition.h"

namespace llm_edgeflow {

const std::unordered_set<std::string>& ValidCardinalities();
const std::unordered_set<std::string>& ValidProvenance();
const std::unordered_set<std::string>& ValidLifetimes();

template <typename Port>
bool ValidatePortDefinitions(const std::vector<Port>& ports,
                             std::unordered_set<std::string>* seen_keys,
                             std::string* error) {
  for (const auto& port : ports) {
    if (port.Name().empty()) {
      if (error) *error = "Port key cannot be empty";
      return false;
    }
    if (port.type_id.empty()) {
      if (error)
        *error = "Port type_id cannot be empty for port: " + port.Name();
      return false;
    }
    if (!ValidCardinalities().count(port.cardinality)) {
      if (error) {
        *error = "Invalid port cardinality '" + port.cardinality +
                 "' in port: " + port.Name();
      }
      return false;
    }
    if (!ValidProvenance().count(port.provenance_policy)) {
      if (error) {
        *error = "Invalid port provenance policy '" + port.provenance_policy +
                 "' in port: " + port.Name();
      }
      return false;
    }
    if (!ValidLifetimes().count(port.lifetime)) {
      if (error) {
        *error = "Invalid port lifetime '" + port.lifetime +
                 "' in port: " + port.Name();
      }
      return false;
    }
    if (seen_keys && !seen_keys->insert(port.Name()).second) {
      if (error) *error = "Duplicate port key: " + port.Name();
      return false;
    }
  }
  return true;
}

bool ValidateNodeDefinitionStructure(const NodeDefinition& definition,
                                     std::string* error = nullptr);

}  // namespace llm_edgeflow

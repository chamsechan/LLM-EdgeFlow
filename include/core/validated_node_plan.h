#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/pipeline_config.h"

namespace llm_edgeflow {

enum class PortDirection { kInput, kOutput };

struct ResolvedPortBinding {
  std::string logical_name;
  std::string blackboard_key;
  std::string type_id;
  std::string cardinality;
  std::string provenance_policy;
  std::string lifetime;
  PortDirection direction = PortDirection::kInput;
};

struct ValidatedNodePlan {
  ParsedNodeConfig node;
  nlohmann::json normalized_config;
  std::vector<ResolvedPortBinding> ports;

  std::string FindPortKey(
      const std::string& logical_name,
      PortDirection direction = PortDirection::kInput) const {
    for (const auto& port : ports) {
      if (port.logical_name == logical_name && port.direction == direction) {
        return port.blackboard_key;
      }
    }
    return {};
  }

  const ResolvedPortBinding* FindPort(
      const std::string& logical_name,
      PortDirection direction = PortDirection::kInput) const {
    for (const auto& port : ports) {
      if (port.logical_name == logical_name && port.direction == direction) {
        return &port;
      }
    }
    return nullptr;
  }
};

}  // namespace llm_edgeflow

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

struct ResolvedNodeModelBinding {
  std::string name;
  std::string capability;
  std::string config_field;
  std::string model_id;
};

struct ValidatedNodePlan {
  ParsedNodeConfig node;
  nlohmann::json normalized_config;
  std::vector<ResolvedPortBinding> ports;
  std::vector<ResolvedNodeModelBinding> model_bindings;

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

  const ResolvedNodeModelBinding* FindModelBinding(
      const std::string& name) const {
    for (const auto& binding : model_bindings) {
      if (binding.name == name) {
        return &binding;
      }
    }
    return nullptr;
  }
};

}  // namespace llm_edgeflow

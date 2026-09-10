#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

#include "contracts/config_schema.h"
#include "core/port_definition.h"

namespace llm_edgeflow {

enum class PortConstraintKind {
  kAtLeastOneOf = 0,
  kExactlyOneOf = 1,
  kAllOrNone = 2,
  kAtMostOneOf = 3,
  kExactOneGroupOf = 4,
};

struct PortGroupConstraint {
  PortConstraintKind kind = PortConstraintKind::kAtLeastOneOf;
  std::vector<std::string> ports;
  std::vector<std::vector<std::string>> port_groups;
  std::string message;

  PortGroupConstraint() = default;
  PortGroupConstraint(PortConstraintKind k, std::vector<std::string> p,
                      std::string msg = {})
      : kind(k), ports(std::move(p)), message(std::move(msg)) {}

  static PortGroupConstraint Groups(
      PortConstraintKind k, std::vector<std::vector<std::string>> groups,
      std::string msg = {}) {
    PortGroupConstraint c;
    c.kind = k;
    c.port_groups = std::move(groups);
    c.message = std::move(msg);
    return c;
  }
};

struct ControlCommandDefinition {
  int cmd_id = 0;
  std::string name;
  std::string description;
  nlohmann::json payload_schema = nlohmann::json::object();
  bool supports_hot_swap = false;
  // Opt in on both definitions when sharing an identical command across types.
  bool shared_id = false;

  ControlCommandDefinition() = default;
  ControlCommandDefinition(int id, std::string n, std::string desc = {},
                           nlohmann::json schema = nlohmann::json::object(),
                           bool hot_swap = false)
      : cmd_id(id),
        name(std::move(n)),
        description(std::move(desc)),
        payload_schema(std::move(schema)),
        supports_hot_swap(hot_swap) {}
};

using NodeConfigValidator =
    std::function<bool(const nlohmann::json&,
                       const std::unordered_set<std::string>&, std::string*)>;

struct NodeDefinition {
  std::string node_type;
  std::string category;
  std::string description;
  std::vector<NodePortDefinition> inputs;
  std::vector<NodePortDefinition> outputs;
  std::vector<PortGroupConstraint> port_constraints;
  std::vector<ControlCommandDefinition> control_commands;
  std::vector<ConfigFieldDefinition> config_fields;
  // Pure semantic validation: no model/session allocation or external I/O.
  NodeConfigValidator validate_config;
  std::string model_capability;
  std::string model_config_field;
  bool parallel_safe = false;
  std::vector<std::string> biz_names;
};

const char* PortConstraintKindName(PortConstraintKind kind);

}  // namespace llm_edgeflow

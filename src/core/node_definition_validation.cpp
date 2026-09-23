#include "core/node_definition_validation.h"

#include <algorithm>
#include <unordered_set>

#include "contracts/config_schema_validation.h"
#include "contracts/control_payload.h"

namespace llm_edgeflow {

const std::unordered_set<std::string>& ValidCardinalities() {
  static const std::unordered_set<std::string> kValidCardinalities = {
      "1:1", "1:N", "N:1", "N:M"};
  return kValidCardinalities;
}

const std::unordered_set<std::string>& ValidProvenance() {
  static const std::unordered_set<std::string> kValidProvenance = {
      "preserve", "generate_sub_id", "aggregate", "independent"};
  return kValidProvenance;
}

const std::unordered_set<std::string>& ValidLifetimes() {
  static const std::unordered_set<std::string> kValidLifetimes = {
      "request", "session", "global"};
  return kValidLifetimes;
}

bool ValidateNodeDefinitionStructure(const NodeDefinition& definition,
                                     std::string* error) {
  if (error) {
    *error = "Invalid or duplicate NodeDefinition: " + definition.node_type;
  }
  if (definition.node_type.empty()) return false;

  std::unordered_set<std::string> seen_in_ports;
  if (!ValidatePortDefinitions(definition.inputs, &seen_in_ports, error)) {
    return false;
  }
  std::unordered_set<std::string> seen_out_ports;
  if (!ValidatePortDefinitions(definition.outputs, &seen_out_ports, error)) {
    return false;
  }
  for (const auto& constraint : definition.port_constraints) {
    if (constraint.kind == PortConstraintKind::kExactOneGroupOf) {
      if (constraint.port_groups.empty()) return false;
      for (const auto& group : constraint.port_groups) {
        if (group.empty()) return false;
        for (const auto& p : group) {
          if (!seen_in_ports.count(p) && !seen_out_ports.count(p)) return false;
        }
      }
    } else {
      if (constraint.ports.empty()) return false;
      for (const auto& p : constraint.ports) {
        if (!seen_in_ports.count(p) && !seen_out_ports.count(p)) return false;
      }
    }
  }
  std::unordered_set<int> seen_cmd_ids;
  std::unordered_set<std::string> seen_cmd_names;
  for (const auto& cmd : definition.control_commands) {
    if (cmd.cmd_id <= 0 || cmd.name.empty()) return false;
    std::string schema_error;
    if (!ValidateControlSchema(cmd.payload_schema, &schema_error)) {
      if (error) {
        *error = "Node '" + definition.node_type + "', Control " +
                 std::to_string(cmd.cmd_id) + " ('" + cmd.name +
                 "'): " + schema_error;
      }
      return false;
    }
    if (!seen_cmd_ids.insert(cmd.cmd_id).second) return false;
    if (!seen_cmd_names.insert(cmd.name).second) return false;
  }
  std::string field_err;
  if (!ValidateConfigFieldDefinitions(definition.config_fields, &field_err)) {
    if (error) *error = field_err;
    return false;
  }
  const auto validates_lifetime_override = [&](const NodePortDefinition& port) {
    if (port.lifetime_config_field.empty()) return true;
    auto it = std::find_if(
        definition.config_fields.begin(), definition.config_fields.end(),
        [&](const auto& f) { return f.name == port.lifetime_config_field; });
    if (it == definition.config_fields.end() ||
        it->kind != ConfigValueKind::kString || it->enum_values.empty()) {
      return false;
    }
    return std::all_of(
        it->enum_values.begin(), it->enum_values.end(),
        [&](const auto& value) { return ValidLifetimes().count(value) != 0; });
  };
  if (!std::all_of(definition.inputs.begin(), definition.inputs.end(),
                   validates_lifetime_override) ||
      !std::all_of(definition.outputs.begin(), definition.outputs.end(),
                   validates_lifetime_override)) {
    return false;
  }
  std::unordered_set<std::string> seen_dep_names;
  std::unordered_set<std::string> seen_dep_config_fields;
  for (const auto& dep : definition.model_dependencies) {
    if (dep.name.empty() || dep.capability.empty() ||
        dep.config_field.empty()) {
      if (error) {
        *error =
            "Model dependency name, capability, and config_field must be "
            "non-empty";
      }
      return false;
    }
    if (!seen_dep_names.insert(dep.name).second) {
      if (error) *error = "Duplicate model dependency name: " + dep.name;
      return false;
    }
    if (!seen_dep_config_fields.insert(dep.config_field).second) {
      if (error) {
        *error = "Duplicate model dependency config_field: " + dep.config_field;
      }
      return false;
    }
    auto it = std::find_if(
        definition.config_fields.begin(), definition.config_fields.end(),
        [&](const auto& f) { return f.name == dep.config_field; });
    if (it == definition.config_fields.end()) {
      if (error) {
        *error = "Model dependency config_field '" + dep.config_field +
                 "' not found in config_fields";
      }
      return false;
    }
    if (it->kind != ConfigValueKind::kString) {
      if (error) {
        *error = "Model dependency config_field '" + dep.config_field +
                 "' must be of string kind";
      }
      return false;
    }
    if (!it->required || !it->default_value.is_null()) {
      if (error) {
        *error = "Model dependency config_field '" + dep.config_field +
                 "' must be required and have no default model reference";
      }
      return false;
    }
  }
  if (error) error->clear();
  return true;
}

}  // namespace llm_edgeflow

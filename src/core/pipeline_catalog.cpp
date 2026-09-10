#include "core/pipeline_catalog.h"

#include <algorithm>
#include <mutex>
#include <unordered_set>
#include <utility>

#include "contracts/config_schema_validation.h"
#include "contracts/control_payload.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {
namespace {

using Kind = ConfigValueKind;

std::vector<NodeDefinition>& RegisteredNodes() {
  static std::vector<NodeDefinition> definitions;
  return definitions;
}

std::vector<BizDefinition>& RegisteredBizs() {
  static std::vector<BizDefinition> definitions;
  return definitions;
}

std::mutex& CatalogMutex() {
  static std::mutex mutex;
  return mutex;
}

template <typename Port>
nlohmann::json PortJson(const Port& port) {
  nlohmann::json result = {{"key", port.Name()},
                           {"type_id", port.type_id},
                           {"required", port.required},
                           {"cardinality", port.cardinality},
                           {"provenance_policy", port.provenance_policy},
                           {"lifetime", port.lifetime}};
  if (!port.lifetime_config_field.empty()) {
    result["lifetime_config_field"] = port.lifetime_config_field;
  }
  return result;
}

nlohmann::json ConstraintJson(const PortGroupConstraint& constraint) {
  nlohmann::json result = {{"kind", PortConstraintKindName(constraint.kind)},
                           {"message", constraint.message}};
  if (constraint.kind == PortConstraintKind::kExactOneGroupOf) {
    result["port_groups"] = constraint.port_groups;
  } else {
    result["ports"] = constraint.ports;
  }
  return result;
}

nlohmann::json ControlCommandJson(const ControlCommandDefinition& cmd) {
  return {{"cmd_id", cmd.cmd_id},
          {"name", cmd.name},
          {"description", cmd.description},
          {"payload_schema", cmd.payload_schema},
          {"supports_hot_swap", cmd.supports_hot_swap},
          {"shared_id", cmd.shared_id}};
}

nlohmann::json FieldJson(const ConfigFieldDefinition& field) {
  nlohmann::json result = {{"name", field.name},
                           {"type", ConfigValueKindName(field.kind)},
                           {"required", field.required}};
  if (!field.default_value.is_null()) result["default"] = field.default_value;
  if (field.minimum) result["minimum"] = *field.minimum;
  if (field.maximum) result["maximum"] = *field.maximum;
  if (!field.enum_values.empty()) result["enum"] = field.enum_values;
  if (!field.semantic.empty()) result["semantic"] = field.semantic;
  return result;
}

}  // namespace

const char* PortConstraintKindName(PortConstraintKind kind) {
  switch (kind) {
    case PortConstraintKind::kAtLeastOneOf:
      return "at_least_one_of";
    case PortConstraintKind::kExactlyOneOf:
      return "exactly_one_of";
    case PortConstraintKind::kAllOrNone:
      return "all_or_none";
    case PortConstraintKind::kAtMostOneOf:
      return "at_most_one_of";
    case PortConstraintKind::kExactOneGroupOf:
      return "exact_one_group_of";
  }
  return "unknown";
}

namespace {
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

}  // namespace

bool PipelineCatalog::RegisterNodeDefinition(const NodeDefinition& definition,
                                             std::string* error) {
  if (error)
    *error = "Invalid or duplicate NodeDefinition: " + definition.node_type;
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
  if (!definition.model_capability.empty()) {
    if (definition.model_config_field.empty()) return false;
    auto it = std::find_if(
        definition.config_fields.begin(), definition.config_fields.end(),
        [&](const auto& f) { return f.name == definition.model_config_field; });
    if (it == definition.config_fields.end() ||
        it->kind != ConfigValueKind::kString) {
      return false;
    }
  }
  std::lock_guard<std::mutex> lock(CatalogMutex());
  auto& definitions = RegisteredNodes();
  if (std::any_of(definitions.begin(), definitions.end(),
                  [&](const auto& item) {
                    return item.node_type == definition.node_type;
                  })) {
    return false;
  }
  for (const auto& existing : definitions) {
    for (const auto& command : definition.control_commands) {
      for (const auto& registered : existing.control_commands) {
        if (command.cmd_id != registered.cmd_id) continue;
        if (!command.shared_id || !registered.shared_id ||
            command.name != registered.name ||
            command.payload_schema != registered.payload_schema ||
            command.supports_hot_swap != registered.supports_hot_swap) {
          if (error) {
            *error =
                "Control ID " + std::to_string(command.cmd_id) +
                " conflicts between " + existing.node_type + " and " +
                definition.node_type +
                "; shared commands require shared_id and identical contracts";
          }
          return false;
        }
      }
    }
  }
  definitions.push_back(definition);
  std::sort(definitions.begin(), definitions.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.node_type < rhs.node_type;
            });
  if (error) error->clear();
  return true;
}

bool PipelineCatalog::RegisterBizDefinition(const BizDefinition& definition) {
  return RegisterBizDefinitions({definition});
}

bool PipelineCatalog::RegisterBizDefinitions(
    const std::vector<BizDefinition>& batch) {
  if (batch.empty()) return false;
  std::lock_guard<std::mutex> lock(CatalogMutex());
  auto& definitions = RegisteredBizs();
  std::vector<std::string> batch_names;
  batch_names.reserve(batch.size());
  for (const auto& definition : batch) {
    if (definition.biz_name.empty()) return false;
    if (std::find(batch_names.begin(), batch_names.end(),
                  definition.biz_name) != batch_names.end()) {
      return false;
    }
    if (std::any_of(definitions.begin(), definitions.end(),
                    [&](const auto& item) {
                      return item.biz_name == definition.biz_name;
                    })) {
      return false;
    }
    std::unordered_set<std::string> seen_ingress;
    if (!ValidatePortDefinitions(definition.ingress, &seen_ingress, nullptr)) {
      return false;
    }
    std::unordered_set<std::string> seen_egress;
    if (!ValidatePortDefinitions(definition.egress, &seen_egress, nullptr)) {
      return false;
    }
    batch_names.push_back(definition.biz_name);
  }
  definitions.insert(definitions.end(), batch.begin(), batch.end());
  std::sort(definitions.begin(), definitions.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.biz_name < rhs.biz_name;
            });
  return true;
}

const NodeDefinition* PipelineCatalogSnapshot::FindNode(
    const std::string& node_type) const {
  auto it = std::find_if(nodes.begin(), nodes.end(), [&](const auto& item) {
    return item.node_type == node_type;
  });
  return it == nodes.end() ? nullptr : &*it;
}

const BizDefinition* PipelineCatalogSnapshot::FindBiz(
    const std::string& biz_name) const {
  auto it = std::find_if(bizs.begin(), bizs.end(), [&](const auto& item) {
    return item.biz_name == biz_name;
  });
  return it == bizs.end() ? nullptr : &*it;
}

PipelineCatalogSnapshot PipelineCatalog::Snapshot() {
  std::lock_guard<std::mutex> lock(CatalogMutex());
  return {RegisteredNodes(), RegisteredBizs()};
}

std::vector<NodeDefinition> PipelineCatalog::Nodes() {
  std::lock_guard<std::mutex> lock(CatalogMutex());
  return RegisteredNodes();
}

std::vector<ModelDefinition> PipelineCatalog::Models() {
  return ModelRegistry::Instance().ListDefinitions();
}

std::vector<BackendDefinition> PipelineCatalog::Backends() {
  return BackendRegistry::Instance().ListDefinitions();
}

std::vector<BizDefinition> PipelineCatalog::Bizs() {
  std::lock_guard<std::mutex> lock(CatalogMutex());
  return RegisteredBizs();
}

std::optional<NodeDefinition> PipelineCatalog::FindNode(
    const std::string& node_type) {
  std::lock_guard<std::mutex> lock(CatalogMutex());
  const auto& nodes = RegisteredNodes();
  auto it = std::find_if(nodes.begin(), nodes.end(), [&](const auto& item) {
    return item.node_type == node_type;
  });
  if (it == nodes.end()) return std::nullopt;
  return *it;
}

std::optional<ModelDefinition> PipelineCatalog::FindModel(
    const std::string& model_type) {
  return ModelRegistry::Instance().Find(model_type);
}

std::optional<BackendDefinition> PipelineCatalog::FindBackend(
    const std::string& backend_type) {
  return BackendRegistry::Instance().Find(backend_type);
}

std::optional<BizDefinition> PipelineCatalog::FindBiz(
    const std::string& biz_name) {
  std::lock_guard<std::mutex> lock(CatalogMutex());
  const auto& bizs = RegisteredBizs();
  auto it = std::find_if(bizs.begin(), bizs.end(), [&](const auto& item) {
    return item.biz_name == biz_name;
  });
  if (it == bizs.end()) return std::nullopt;
  return *it;
}

void PipelineCatalog::ClearForTesting() {
  std::lock_guard<std::mutex> lock(CatalogMutex());
  RegisteredNodes().clear();
  RegisteredBizs().clear();
}

nlohmann::json PipelineCatalog::NodeToJson(const NodeDefinition& definition) {
  nlohmann::json inputs = nlohmann::json::array();
  nlohmann::json outputs = nlohmann::json::array();
  nlohmann::json constraints = nlohmann::json::array();
  nlohmann::json commands = nlohmann::json::array();
  nlohmann::json fields = nlohmann::json::array();
  for (const auto& item : definition.inputs) inputs.push_back(PortJson(item));
  for (const auto& item : definition.outputs) outputs.push_back(PortJson(item));
  for (const auto& item : definition.port_constraints)
    constraints.push_back(ConstraintJson(item));
  for (const auto& item : definition.control_commands)
    commands.push_back(ControlCommandJson(item));
  for (const auto& item : definition.config_fields)
    fields.push_back(FieldJson(item));
  return {{"node_type", definition.node_type},
          {"category", definition.category},
          {"description", definition.description},
          {"inputs", std::move(inputs)},
          {"outputs", std::move(outputs)},
          {"port_constraints", std::move(constraints)},
          {"control_commands", std::move(commands)},
          {"config_fields", std::move(fields)},
          {"model_capability", definition.model_capability},
          {"model_config_field", definition.model_config_field},
          {"parallel_safe", definition.parallel_safe},
          {"biz_names", definition.biz_names}};
}

nlohmann::json PipelineCatalog::ModelToJson(const ModelDefinition& definition) {
  nlohmann::json fields = nlohmann::json::array();
  for (const auto& field : definition.config_fields) {
    fields.push_back(FieldJson(field));
  }
  return {
      {"model_type", definition.model_type},
      {"capability", definition.capability},
      {"description", definition.description},
      {"required_protocol",
       ExecutionProtocolName(definition.required_protocol)},
      {"concurrency", InferenceConcurrencyName(definition.concurrency)},
      {"config_fields", std::move(fields)},
  };
}

nlohmann::json PipelineCatalog::BackendToJson(
    const BackendDefinition& definition) {
  nlohmann::json fields = nlohmann::json::array();
  for (const auto& field : definition.config_fields) {
    fields.push_back(FieldJson(field));
  }
  nlohmann::json protocols = nlohmann::json::array();
  for (auto p : definition.supported_protocols) {
    protocols.push_back(ExecutionProtocolName(p));
  }
  return {
      {"backend_type", definition.backend_type},
      {"description", definition.description},
      {"supported_protocols", std::move(protocols)},
      {"concurrency", InferenceConcurrencyName(definition.concurrency)},
      {"config_fields", std::move(fields)},
  };
}

nlohmann::json PipelineCatalog::ToJson(const std::string& biz_filter) {
  const auto snapshot = Snapshot();
  nlohmann::json nodes = nlohmann::json::array();
  for (const auto& item : snapshot.nodes) {
    if (!biz_filter.empty() && !item.biz_names.empty() &&
        std::find(item.biz_names.begin(), item.biz_names.end(), biz_filter) ==
            item.biz_names.end()) {
      continue;
    }
    nodes.push_back(NodeToJson(item));
  }

  nlohmann::json models = nlohmann::json::array();
  for (const auto& item : Models()) {
    models.push_back(ModelToJson(item));
  }

  nlohmann::json backends = nlohmann::json::array();
  for (const auto& item : Backends()) {
    backends.push_back(BackendToJson(item));
  }

  nlohmann::json bizs = nlohmann::json::array();
  for (const auto& item : snapshot.bizs) {
    if (!biz_filter.empty() && item.biz_name != biz_filter) continue;
    nlohmann::json ingress = nlohmann::json::array();
    nlohmann::json egress = nlohmann::json::array();
    for (const auto& port : item.ingress) ingress.push_back(PortJson(port));
    for (const auto& port : item.egress) egress.push_back(PortJson(port));
    bizs.push_back({{"biz_name", item.biz_name},
                    {"demo_biz", item.demo_biz},
                    {"display_name", item.display_name},
                    {"ingress", std::move(ingress)},
                    {"egress", std::move(egress)}});
  }

  return {{"schema_version", 2},
          {"nodes", std::move(nodes)},
          {"models", std::move(models)},
          {"backends", std::move(backends)},
          {"bizs", std::move(bizs)}};
}

}  // namespace llm_edgeflow

#include "core/pipeline_catalog.h"

#include <algorithm>
#include <mutex>
#include <unordered_set>
#include <utility>

namespace alg_framework {
namespace {

using Kind = ConfigValueKind;

std::vector<NodeDefinition>& RegisteredNodes() {
  static std::vector<NodeDefinition> definitions;
  return definitions;
}

std::vector<EngineDefinition>& RegisteredEngines() {
  static std::vector<EngineDefinition> definitions;
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

nlohmann::json PortJson(const PortDefinition& port) {
  return {{"key", port.key},
          {"type_id", port.type_id},
          {"required", port.required},
          {"allow_override", port.allow_override},
          {"cardinality", port.cardinality},
          {"provenance_policy", port.provenance_policy},
          {"lifetime", port.lifetime}};
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
          {"supports_hot_swap", cmd.supports_hot_swap}};
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

const char* ConfigValueKindName(ConfigValueKind kind) {
  switch (kind) {
    case ConfigValueKind::kString:
      return "string";
    case ConfigValueKind::kInteger:
      return "integer";
    case ConfigValueKind::kNumber:
      return "number";
    case ConfigValueKind::kBoolean:
      return "boolean";
    case ConfigValueKind::kObject:
      return "object";
    case ConfigValueKind::kArray:
      return "array";
  }
  return "unknown";
}

const char* EngineThreadModelName(EngineThreadModel model) {
  switch (model) {
    case EngineThreadModel::kSerialized:
      return "serialized";
    case EngineThreadModel::kConcurrent:
      return "concurrent";
  }
  return "unknown";
}

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

bool MatchesKind(const nlohmann::json& value, ConfigValueKind kind) {
  switch (kind) {
    case ConfigValueKind::kString:
      return value.is_string();
    case ConfigValueKind::kInteger:
      return value.is_number_integer();
    case ConfigValueKind::kNumber:
      return value.is_number();
    case ConfigValueKind::kBoolean:
      return value.is_boolean();
    case ConfigValueKind::kObject:
      return value.is_object();
    case ConfigValueKind::kArray:
      return value.is_array();
  }
  return false;
}

bool ValidateFieldDefinition(const ConfigFieldDefinition& field,
                             std::unordered_set<std::string>& seen_names) {
  if (field.name.empty()) return false;
  if (!seen_names.insert(field.name).second) return false;
  if (field.kind != ConfigValueKind::kInteger &&
      field.kind != ConfigValueKind::kNumber) {
    if (field.minimum.has_value() || field.maximum.has_value()) {
      return false;
    }
  }
  if (field.minimum.has_value() && field.maximum.has_value() &&
      *field.minimum > *field.maximum) {
    return false;
  }
  if (!field.enum_values.empty()) {
    if (field.kind != ConfigValueKind::kString) return false;
    std::unordered_set<std::string> seen_enums;
    for (const auto& ev : field.enum_values) {
      if (ev.empty() || !seen_enums.insert(ev).second) return false;
    }
  }
  if (!field.default_value.is_null()) {
    if (!MatchesKind(field.default_value, field.kind)) return false;
    if (field.kind == ConfigValueKind::kInteger ||
        field.kind == ConfigValueKind::kNumber) {
      double val = field.default_value.get<double>();
      if (field.minimum.has_value() && val < *field.minimum) return false;
      if (field.maximum.has_value() && val > *field.maximum) return false;
    } else if (field.kind == ConfigValueKind::kString &&
               !field.enum_values.empty()) {
      std::string val = field.default_value.get<std::string>();
      if (std::find(field.enum_values.begin(), field.enum_values.end(), val) ==
          field.enum_values.end()) {
        return false;
      }
    }
  }
  return true;
}

bool PipelineCatalog::RegisterNodeDefinition(const NodeDefinition& definition) {
  if (definition.node_type.empty()) return false;
  static const std::unordered_set<std::string> kValidCardinalities = {
      "1:1", "1:N", "N:1", "N:M"};
  static const std::unordered_set<std::string> kValidProvenance = {
      "preserve", "generate_sub_id", "aggregate", "independent"};
  static const std::unordered_set<std::string> kValidLifetimes = {
      "request", "session", "global"};

  std::unordered_set<std::string> seen_in_ports;
  for (const auto& port : definition.inputs) {
    if (port.key.empty() || port.type_id.empty()) return false;
    if (!port.cardinality.empty() &&
        !kValidCardinalities.count(port.cardinality))
      return false;
    if (!port.provenance_policy.empty() &&
        !kValidProvenance.count(port.provenance_policy))
      return false;
    if (!port.lifetime.empty() && !kValidLifetimes.count(port.lifetime))
      return false;
    if (!seen_in_ports.insert(port.key).second) return false;
  }
  std::unordered_set<std::string> seen_out_ports;
  for (const auto& port : definition.outputs) {
    if (port.key.empty() || port.type_id.empty()) return false;
    if (!port.cardinality.empty() &&
        !kValidCardinalities.count(port.cardinality))
      return false;
    if (!port.provenance_policy.empty() &&
        !kValidProvenance.count(port.provenance_policy))
      return false;
    if (!port.lifetime.empty() && !kValidLifetimes.count(port.lifetime))
      return false;
    if (!seen_out_ports.insert(port.key).second) return false;
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
    if (!seen_cmd_ids.insert(cmd.cmd_id).second) return false;
    if (!seen_cmd_names.insert(cmd.name).second) return false;
  }
  std::unordered_set<std::string> seen_field_names;
  for (const auto& field : definition.config_fields) {
    if (!ValidateFieldDefinition(field, seen_field_names)) return false;
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
  definitions.push_back(definition);
  std::sort(definitions.begin(), definitions.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.node_type < rhs.node_type;
            });
  return true;
}

bool PipelineCatalog::RegisterEngineDefinition(
    const EngineDefinition& definition) {
  if (definition.engine_type.empty()) return false;
  std::unordered_set<std::string> seen_field_names;
  for (const auto& field : definition.config_fields) {
    if (!ValidateFieldDefinition(field, seen_field_names)) return false;
  }
  std::lock_guard<std::mutex> lock(CatalogMutex());
  auto& definitions = RegisteredEngines();
  if (std::any_of(definitions.begin(), definitions.end(),
                  [&](const auto& item) {
                    return item.engine_type == definition.engine_type;
                  })) {
    return false;
  }
  definitions.push_back(definition);
  std::sort(definitions.begin(), definitions.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.engine_type < rhs.engine_type;
            });
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
    batch_names.push_back(definition.biz_name);
  }
  definitions.insert(definitions.end(), batch.begin(), batch.end());
  std::sort(definitions.begin(), definitions.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.biz_name < rhs.biz_name;
            });
  return true;
}

const std::vector<NodeDefinition>& PipelineCatalog::Nodes() {
  std::lock_guard<std::mutex> lock(CatalogMutex());
  return RegisteredNodes();
}

const std::vector<EngineDefinition>& PipelineCatalog::Engines() {
  std::lock_guard<std::mutex> lock(CatalogMutex());
  return RegisteredEngines();
}

const std::vector<BizDefinition>& PipelineCatalog::Bizs() {
  std::lock_guard<std::mutex> lock(CatalogMutex());
  return RegisteredBizs();
}

const NodeDefinition* PipelineCatalog::FindNode(const std::string& node_type) {
  std::lock_guard<std::mutex> lock(CatalogMutex());
  const auto& nodes = RegisteredNodes();
  auto it = std::find_if(nodes.begin(), nodes.end(), [&](const auto& item) {
    return item.node_type == node_type;
  });
  return it == nodes.end() ? nullptr : &*it;
}

const EngineDefinition* PipelineCatalog::FindEngine(
    const std::string& engine_type) {
  std::lock_guard<std::mutex> lock(CatalogMutex());
  const auto& engines = RegisteredEngines();
  auto it = std::find_if(engines.begin(), engines.end(), [&](const auto& item) {
    return item.engine_type == engine_type;
  });
  return it == engines.end() ? nullptr : &*it;
}

const BizDefinition* PipelineCatalog::FindBiz(const std::string& biz_name) {
  std::lock_guard<std::mutex> lock(CatalogMutex());
  const auto& bizs = RegisteredBizs();
  auto it = std::find_if(bizs.begin(), bizs.end(), [&](const auto& item) {
    return item.biz_name == biz_name;
  });
  return it == bizs.end() ? nullptr : &*it;
}

void PipelineCatalog::ClearForTesting() {
  std::lock_guard<std::mutex> lock(CatalogMutex());
  RegisteredNodes().clear();
  RegisteredEngines().clear();
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
          {"biz_names", definition.biz_names},
          {"business_names", definition.biz_names}};
}

nlohmann::json PipelineCatalog::ToJson(const std::string& biz_filter) {
  nlohmann::json nodes = nlohmann::json::array();
  for (const auto& item : Nodes()) {
    if (!biz_filter.empty() && !item.biz_names.empty() &&
        std::find(item.biz_names.begin(), item.biz_names.end(), biz_filter) ==
            item.biz_names.end()) {
      continue;
    }
    nodes.push_back(NodeToJson(item));
  }

  nlohmann::json engines = nlohmann::json::array();
  for (const auto& item : Engines()) {
    nlohmann::json fields = nlohmann::json::array();
    for (const auto& field : item.config_fields)
      fields.push_back(FieldJson(field));
    engines.push_back(
        {{"engine_type", item.engine_type},
         {"capability", item.capability},
         {"description", item.description},
         {"thread_model", EngineThreadModelName(item.thread_model)},
         {"config_fields", std::move(fields)}});
  }

  nlohmann::json bizs = nlohmann::json::array();
  for (const auto& item : Bizs()) {
    if (!biz_filter.empty() && item.biz_name != biz_filter) continue;
    nlohmann::json ingress = nlohmann::json::array();
    nlohmann::json egress = nlohmann::json::array();
    for (const auto& port : item.ingress) ingress.push_back(PortJson(port));
    for (const auto& port : item.egress) egress.push_back(PortJson(port));
    bizs.push_back({{"biz_name", item.biz_name},
                    {"business_name", item.biz_name},
                    {"demo_biz", item.demo_biz},
                    {"demo_business", item.demo_biz},
                    {"display_name", item.display_name},
                    {"ingress", std::move(ingress)},
                    {"egress", std::move(egress)}});
  }

  return {{"schema_version", 1},
          {"nodes", std::move(nodes)},
          {"engines", std::move(engines)},
          {"bizs", bizs},
          {"businesses", std::move(bizs)}};
}

}  // namespace alg_framework

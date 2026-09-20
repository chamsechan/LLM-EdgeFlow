#include "core/pipeline_catalog.h"

#include <algorithm>
#include <mutex>
#include <unordered_set>
#include <utility>

#include "contracts/config_schema_validation.h"
#include "contracts/control_payload.h"
#include "core/node_definition_validation.h"
#include "core/node_registry.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {
namespace {

std::vector<BizDefinition>& RegisteredBizs() {
  static std::vector<BizDefinition> definitions;
  return definitions;
}

std::mutex& BizMutex() {
  static std::mutex mutex;
  return mutex;
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

nlohmann::json PipelineCatalog::PortToJson(const std::string& key,
                                           const PortContract& port) {
  nlohmann::json result = {{"key", key},
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

bool PipelineCatalog::RegisterBizDefinition(const BizDefinition& definition) {
  return RegisterBizDefinitions({definition});
}

bool PipelineCatalog::RegisterBizDefinitions(
    const std::vector<BizDefinition>& batch) {
  if (batch.empty()) return false;
  std::lock_guard<std::mutex> lock(BizMutex());
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
  auto node_snapshot = NodeRegistry::Instance().Snapshot();
  std::vector<BizDefinition> bizs;
  {
    std::lock_guard<std::mutex> lock(BizMutex());
    bizs = RegisteredBizs();
  }
  return {std::move(node_snapshot.definitions), std::move(bizs),
          node_snapshot.has_conflict, std::move(node_snapshot.conflict_errors)};
}

std::vector<NodeDefinition> PipelineCatalog::Nodes() {
  return NodeRegistry::Instance().ListDefinitions();
}

std::vector<ModelDefinition> PipelineCatalog::Models() {
  return ModelRegistry::Instance().ListDefinitions();
}

std::vector<BackendDefinition> PipelineCatalog::Backends() {
  return BackendRegistry::Instance().ListDefinitions();
}

std::vector<BizDefinition> PipelineCatalog::Bizs() {
  std::lock_guard<std::mutex> lock(BizMutex());
  return RegisteredBizs();
}

std::optional<NodeDefinition> PipelineCatalog::FindNode(
    const std::string& node_type) {
  return NodeRegistry::Instance().Find(node_type);
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
  std::lock_guard<std::mutex> lock(BizMutex());
  const auto& bizs = RegisteredBizs();
  auto it = std::find_if(bizs.begin(), bizs.end(), [&](const auto& item) {
    return item.biz_name == biz_name;
  });
  if (it == bizs.end()) return std::nullopt;
  return *it;
}

void PipelineCatalog::ResetBizsForTesting() {
  std::lock_guard<std::mutex> lock(BizMutex());
  RegisteredBizs().clear();
}

nlohmann::json PipelineCatalog::NodeToJson(const NodeDefinition& definition) {
  nlohmann::json inputs = nlohmann::json::array();
  nlohmann::json outputs = nlohmann::json::array();
  nlohmann::json constraints = nlohmann::json::array();
  nlohmann::json commands = nlohmann::json::array();
  nlohmann::json fields = nlohmann::json::array();
  for (const auto& item : definition.inputs)
    inputs.push_back(PortToJson(item.Name(), item));
  for (const auto& item : definition.outputs)
    outputs.push_back(PortToJson(item.Name(), item));
  for (const auto& item : definition.port_constraints)
    constraints.push_back(ConstraintJson(item));
  for (const auto& item : definition.control_commands)
    commands.push_back(ControlCommandJson(item));
  for (const auto& item : definition.config_fields)
    fields.push_back(FieldJson(item));
  nlohmann::json model_deps = nlohmann::json::array();
  for (const auto& dep : definition.model_dependencies) {
    model_deps.push_back({
        {"name", dep.name},
        {"capability", dep.capability},
        {"config_field", dep.config_field},
    });
  }
  return {{"node_type", definition.node_type},
          {"category", definition.category},
          {"description", definition.description},
          {"parallel_safe", definition.parallel_safe},
          {"inputs", std::move(inputs)},
          {"outputs", std::move(outputs)},
          {"port_constraints", std::move(constraints)},
          {"control_commands", std::move(commands)},
          {"config_fields", std::move(fields)},
          {"model_dependencies", std::move(model_deps)},
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

nlohmann::json PipelineCatalog::ToJson(const PipelineCatalogSnapshot& snapshot,
                                       const std::string& biz_filter) {
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
    for (const auto& port : item.ingress)
      ingress.push_back(PortToJson(port.Name(), port));
    for (const auto& port : item.egress)
      egress.push_back(PortToJson(port.Name(), port));
    bizs.push_back({{"biz_name", item.biz_name},
                    {"demo_biz", item.demo_biz},
                    {"display_name", item.display_name},
                    {"ingress", std::move(ingress)},
                    {"egress", std::move(egress)}});
  }

  return {{"nodes", std::move(nodes)},
          {"models", std::move(models)},
          {"backends", std::move(backends)},
          {"bizs", std::move(bizs)}};
}

nlohmann::json PipelineCatalog::ToJson(const std::string& biz_filter) {
  return ToJson(Snapshot(), biz_filter);
}

}  // namespace llm_edgeflow

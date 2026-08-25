#include "core/pipeline_catalog.h"

#include <algorithm>
#include <mutex>
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

std::vector<BusinessDefinition>& RegisteredBusinesses() {
  static std::vector<BusinessDefinition> definitions;
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
          {"allow_override", port.allow_override}};
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

bool PipelineCatalog::RegisterNodeDefinition(const NodeDefinition& definition) {
  if (definition.node_type.empty()) return false;
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

bool PipelineCatalog::RegisterBusinessDefinition(
    const BusinessDefinition& definition) {
  if (definition.business_name.empty()) return false;
  std::lock_guard<std::mutex> lock(CatalogMutex());
  auto& definitions = RegisteredBusinesses();
  if (std::any_of(definitions.begin(), definitions.end(),
                  [&](const auto& item) {
                    return item.business_name == definition.business_name;
                  })) {
    return false;
  }
  definitions.push_back(definition);
  std::sort(definitions.begin(), definitions.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.business_name < rhs.business_name;
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

const std::vector<BusinessDefinition>& PipelineCatalog::Businesses() {
  std::lock_guard<std::mutex> lock(CatalogMutex());
  return RegisteredBusinesses();
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

const BusinessDefinition* PipelineCatalog::FindBusiness(
    const std::string& business_name) {
  std::lock_guard<std::mutex> lock(CatalogMutex());
  const auto& businesses = RegisteredBusinesses();
  auto it = std::find_if(
      businesses.begin(), businesses.end(),
      [&](const auto& item) { return item.business_name == business_name; });
  return it == businesses.end() ? nullptr : &*it;
}

void PipelineCatalog::ClearForTesting() {
  std::lock_guard<std::mutex> lock(CatalogMutex());
  RegisteredNodes().clear();
  RegisteredEngines().clear();
  RegisteredBusinesses().clear();
}

nlohmann::json PipelineCatalog::NodeToJson(const NodeDefinition& definition) {
  nlohmann::json inputs = nlohmann::json::array();
  nlohmann::json outputs = nlohmann::json::array();
  nlohmann::json fields = nlohmann::json::array();
  for (const auto& item : definition.inputs) inputs.push_back(PortJson(item));
  for (const auto& item : definition.outputs) outputs.push_back(PortJson(item));
  for (const auto& item : definition.config_fields)
    fields.push_back(FieldJson(item));
  return {{"node_type", definition.node_type},
          {"category", definition.category},
          {"description", definition.description},
          {"inputs", std::move(inputs)},
          {"outputs", std::move(outputs)},
          {"config_fields", std::move(fields)},
          {"model_capability", definition.model_capability},
          {"model_config_field", definition.model_config_field},
          {"parallel_safe", definition.parallel_safe},
          {"business_names", definition.business_names}};
}

nlohmann::json PipelineCatalog::ToJson(const std::string& business_filter) {
  nlohmann::json nodes = nlohmann::json::array();
  for (const auto& item : Nodes()) {
    if (!business_filter.empty() && !item.business_names.empty() &&
        std::find(item.business_names.begin(), item.business_names.end(),
                  business_filter) == item.business_names.end()) {
      continue;
    }
    nodes.push_back(NodeToJson(item));
  }

  nlohmann::json engines = nlohmann::json::array();
  for (const auto& item : Engines()) {
    nlohmann::json fields = nlohmann::json::array();
    for (const auto& field : item.config_fields)
      fields.push_back(FieldJson(field));
    engines.push_back({{"engine_type", item.engine_type},
                       {"capability", item.capability},
                       {"description", item.description},
                       {"config_fields", std::move(fields)}});
  }

  nlohmann::json businesses = nlohmann::json::array();
  for (const auto& item : Businesses()) {
    if (!business_filter.empty() && item.business_name != business_filter)
      continue;
    nlohmann::json ingress = nlohmann::json::array();
    nlohmann::json egress = nlohmann::json::array();
    for (const auto& port : item.ingress) ingress.push_back(PortJson(port));
    for (const auto& port : item.egress) egress.push_back(PortJson(port));
    businesses.push_back({{"business_name", item.business_name},
                          {"demo_business", item.demo_business},
                          {"display_name", item.display_name},
                          {"ingress", std::move(ingress)},
                          {"egress", std::move(egress)}});
  }

  return {{"schema_version", 1},
          {"nodes", std::move(nodes)},
          {"engines", std::move(engines)},
          {"businesses", std::move(businesses)}};
}

}  // namespace alg_framework

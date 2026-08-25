#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "core/blackboard_key.h"

namespace alg_framework {

enum class ConfigValueKind {
  kString,
  kInteger,
  kNumber,
  kBoolean,
  kObject,
  kArray,
};

enum class EngineThreadModel {
  kSerialized = 0,
  kConcurrent = 1,
};

struct PortDefinition {
  std::string key;
  std::string type_id;
  bool required = true;
  bool allow_override = false;
};

template <typename T>
inline PortDefinition RequiredInput(const BlackboardKey<T>& key) {
  return PortDefinition{key.name, key.type_id, true, false};
}

template <typename T>
inline PortDefinition OptionalInput(const BlackboardKey<T>& key) {
  return PortDefinition{key.name, key.type_id, false, false};
}

template <typename T>
inline PortDefinition Output(const BlackboardKey<T>& key,
                             bool allow_override = false) {
  return PortDefinition{key.name, key.type_id, true, allow_override};
}

struct ConfigFieldDefinition {
  std::string name;
  ConfigValueKind kind = ConfigValueKind::kString;
  bool required = false;
  nlohmann::json default_value;
  std::optional<double> minimum;
  std::optional<double> maximum;
  std::vector<std::string> enum_values;
  std::string semantic;
};

struct NodeDefinition {
  std::string node_type;
  std::string category;
  std::string description;
  std::vector<PortDefinition> inputs;
  std::vector<PortDefinition> outputs;
  std::vector<ConfigFieldDefinition> config_fields;
  std::string model_capability;
  std::string model_config_field;
  bool parallel_safe = false;
  std::vector<std::string> business_names;
};

struct EngineDefinition {
  std::string engine_type;
  std::string capability;
  std::string description;
  std::vector<ConfigFieldDefinition> config_fields;
  EngineThreadModel thread_model = EngineThreadModel::kSerialized;
};

struct BusinessDefinition {
  std::string business_name;
  std::string demo_business;
  std::string display_name;
  std::vector<PortDefinition> ingress;
  std::vector<PortDefinition> egress;
};

class PipelineCatalog {
 public:
  static bool RegisterNodeDefinition(const NodeDefinition& definition);
  static bool RegisterEngineDefinition(const EngineDefinition& definition);
  static bool RegisterBusinessDefinition(const BusinessDefinition& definition);

  static const std::vector<NodeDefinition>& Nodes();
  static const std::vector<EngineDefinition>& Engines();
  static const std::vector<BusinessDefinition>& Businesses();

  static const NodeDefinition* FindNode(const std::string& node_type);
  static const EngineDefinition* FindEngine(const std::string& engine_type);
  static const BusinessDefinition* FindBusiness(
      const std::string& business_name);

  static void ClearForTesting();

  static nlohmann::json ToJson(
      const std::string& business_filter = std::string());
  static nlohmann::json NodeToJson(const NodeDefinition& definition);
};

const char* ConfigValueKindName(ConfigValueKind kind);

}  // namespace alg_framework

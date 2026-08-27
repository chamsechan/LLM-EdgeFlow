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

  PortDefinition() = default;
  PortDefinition(std::string k, std::string t, bool req = true,
                 bool allow_ovr = false)
      : key(std::move(k)),
        type_id(std::move(t)),
        required(req),
        allow_override(allow_ovr) {}
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

template <typename T>
inline PortDefinition RequiredInputPort(std::string logical_name,
                                        const BlackboardKey<T>& key_type) {
  return PortDefinition{std::move(logical_name), key_type.type_id, true, false};
}

template <typename T>
inline PortDefinition OptionalInputPort(std::string logical_name,
                                        const BlackboardKey<T>& key_type) {
  return PortDefinition{std::move(logical_name), key_type.type_id, false,
                        false};
}

template <typename T>
inline PortDefinition OutputPort(std::string logical_name,
                                 const BlackboardKey<T>& key_type,
                                 bool allow_override = false) {
  return PortDefinition{std::move(logical_name), key_type.type_id, true,
                        allow_override};
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

  ConfigFieldDefinition() = default;
  ConfigFieldDefinition(std::string field_name, ConfigValueKind value_kind,
                        bool is_required = false,
                        nlohmann::json field_default = nlohmann::json(),
                        std::optional<double> field_minimum = std::nullopt,
                        std::optional<double> field_maximum = std::nullopt,
                        std::vector<std::string> allowed_values = {},
                        std::string field_semantic = {})
      : name(std::move(field_name)),
        kind(value_kind),
        required(is_required),
        default_value(std::move(field_default)),
        minimum(field_minimum),
        maximum(field_maximum),
        enum_values(std::move(allowed_values)),
        semantic(std::move(field_semantic)) {}
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
  std::vector<std::string> biz_names;
};

struct EngineDefinition {
  std::string engine_type;
  std::string capability;
  std::string description;
  std::vector<ConfigFieldDefinition> config_fields;
  EngineThreadModel thread_model = EngineThreadModel::kSerialized;
};

struct BizDefinition {
  std::string biz_name;
  std::string demo_biz;
  std::string display_name;
  std::vector<PortDefinition> ingress;
  std::vector<PortDefinition> egress;

  BizDefinition() = default;
  BizDefinition(std::string name, std::string demo, std::string display = {},
                std::vector<PortDefinition> in = {},
                std::vector<PortDefinition> out = {})
      : biz_name(std::move(name)),
        demo_biz(std::move(demo)),
        display_name(std::move(display)),
        ingress(std::move(in)),
        egress(std::move(out)) {}
};

using BusinessDefinition = BizDefinition;

class PipelineCatalog {
 public:
  static bool RegisterNodeDefinition(const NodeDefinition& definition);
  static bool RegisterEngineDefinition(const EngineDefinition& definition);
  static bool RegisterBizDefinition(const BizDefinition& definition);
  static bool RegisterBizDefinitions(
      const std::vector<BizDefinition>& definitions);

  static bool RegisterBusinessDefinition(const BizDefinition& definition) {
    return RegisterBizDefinition(definition);
  }
  static bool RegisterBusinessDefinitions(
      const std::vector<BizDefinition>& definitions) {
    return RegisterBizDefinitions(definitions);
  }

  static const std::vector<NodeDefinition>& Nodes();
  static const std::vector<EngineDefinition>& Engines();
  static const std::vector<BizDefinition>& Bizs();
  static const std::vector<BizDefinition>& Businesses() { return Bizs(); }

  static const NodeDefinition* FindNode(const std::string& node_type);
  static const EngineDefinition* FindEngine(const std::string& engine_type);
  static const BizDefinition* FindBiz(const std::string& biz_name);
  static const BizDefinition* FindBusiness(const std::string& biz_name) {
    return FindBiz(biz_name);
  }

  static void ClearForTesting();

  static nlohmann::json ToJson(const std::string& biz_filter = std::string());
  static nlohmann::json NodeToJson(const NodeDefinition& definition);
};

const char* ConfigValueKindName(ConfigValueKind kind);
const char* EngineThreadModelName(EngineThreadModel model);

}  // namespace alg_framework

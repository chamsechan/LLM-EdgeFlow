#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "contracts/config_schema.h"
#include "core/blackboard_key.h"
#include "engine/inference_definition.h"

namespace llm_edgeflow {

struct PortDefinition {
  std::string key;
  std::string type_id;
  bool required = true;
  std::string cardinality = "1:1";
  std::string provenance_policy = "preserve";
  std::string lifetime = "request";
  // Optional instance config field overriding lifetime (for example the
  // TextEmbeddingNode request/session cache policy).
  std::string lifetime_config_field;

  PortDefinition() = default;
  PortDefinition(std::string k, std::string t, bool req = true,
                 std::string card = "1:1", std::string prov = "preserve",
                 std::string life = "request",
                 std::string life_config_field = {})
      : key(std::move(k)),
        type_id(std::move(t)),
        required(req),
        cardinality(std::move(card)),
        provenance_policy(std::move(prov)),
        lifetime(std::move(life)),
        lifetime_config_field(std::move(life_config_field)) {}
};

template <typename T>
inline PortDefinition RequiredInput(const BlackboardKey<T>& key) {
  return PortDefinition{key.name, key.type_id, true};
}

template <typename T>
inline PortDefinition OptionalInput(const BlackboardKey<T>& key) {
  return PortDefinition{key.name, key.type_id, false};
}

template <typename T>
inline PortDefinition Output(const BlackboardKey<T>& key) {
  return PortDefinition{key.name, key.type_id, true};
}

template <typename T>
inline PortDefinition RequiredInputPort(
    std::string logical_name, const BlackboardKey<T>& key_type,
    std::string cardinality = "1:1", std::string provenance = "preserve",
    std::string lifetime = "request", std::string lifetime_config_field = {}) {
  return PortDefinition{std::move(logical_name),
                        key_type.type_id,
                        true,
                        std::move(cardinality),
                        std::move(provenance),
                        std::move(lifetime),
                        std::move(lifetime_config_field)};
}

template <typename T>
inline PortDefinition OptionalInputPort(
    std::string logical_name, const BlackboardKey<T>& key_type,
    std::string cardinality = "1:1", std::string provenance = "preserve",
    std::string lifetime = "request", std::string lifetime_config_field = {}) {
  return PortDefinition{std::move(logical_name),
                        key_type.type_id,
                        false,
                        std::move(cardinality),
                        std::move(provenance),
                        std::move(lifetime),
                        std::move(lifetime_config_field)};
}

template <typename T>
inline PortDefinition OutputPort(std::string logical_name,
                                 const BlackboardKey<T>& key_type,
                                 std::string cardinality = "1:1",
                                 std::string provenance = "preserve",
                                 std::string lifetime = "request",
                                 std::string lifetime_config_field = {}) {
  return PortDefinition{std::move(logical_name),
                        key_type.type_id,
                        true,
                        std::move(cardinality),
                        std::move(provenance),
                        std::move(lifetime),
                        std::move(lifetime_config_field)};
}

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

struct NodeDefinition {
  std::string node_type;
  std::string category;
  std::string description;
  std::vector<PortDefinition> inputs;
  std::vector<PortDefinition> outputs;
  std::vector<PortGroupConstraint> port_constraints;
  std::vector<ControlCommandDefinition> control_commands;
  std::vector<ConfigFieldDefinition> config_fields;
  std::string model_capability;
  std::string model_config_field;
  bool parallel_safe = false;
  std::vector<std::string> biz_names;
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

struct PipelineCatalogSnapshot {
  std::vector<NodeDefinition> nodes;
  std::vector<BizDefinition> bizs;

  const NodeDefinition* FindNode(const std::string& node_type) const;
  const BizDefinition* FindBiz(const std::string& biz_name) const;
};

class PipelineCatalog {
 public:
  static bool RegisterNodeDefinition(const NodeDefinition& definition);
  static bool RegisterBizDefinition(const BizDefinition& definition);
  static bool RegisterBizDefinitions(
      const std::vector<BizDefinition>& definitions);

  static PipelineCatalogSnapshot Snapshot();
  static std::vector<NodeDefinition> Nodes();
  static std::vector<ModelDefinition> Models();
  static std::vector<BackendDefinition> Backends();
  static std::vector<BizDefinition> Bizs();

  static std::optional<NodeDefinition> FindNode(const std::string& node_type);
  static std::optional<ModelDefinition> FindModel(
      const std::string& model_type);
  static std::optional<BackendDefinition> FindBackend(
      const std::string& backend_type);
  static std::optional<BizDefinition> FindBiz(const std::string& biz_name);

  static void ClearForTesting();

  static nlohmann::json ToJson(const std::string& biz_filter = std::string());
  static nlohmann::json NodeToJson(const NodeDefinition& definition);
  static nlohmann::json ModelToJson(const ModelDefinition& definition);
  static nlohmann::json BackendToJson(const BackendDefinition& definition);
};

const char* PortConstraintKindName(PortConstraintKind kind);

}  // namespace llm_edgeflow

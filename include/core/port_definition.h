#pragma once

#include <string>
#include <utility>

#include "core/blackboard_key.h"

namespace llm_edgeflow {

// Shared flow properties, independent of how a port is named.
struct PortContract {
  std::string type_id;
  bool required = true;
  std::string cardinality = "1:1";
  std::string provenance_policy = "preserve";
  std::string lifetime = "request";
  std::string lifetime_config_field;

  PortContract() = default;
  PortContract(std::string type, bool req = true, std::string card = "1:1",
               std::string provenance = "preserve",
               std::string life = "request", std::string life_config = {})
      : type_id(std::move(type)),
        required(req),
        cardinality(std::move(card)),
        provenance_policy(std::move(provenance)),
        lifetime(std::move(life)),
        lifetime_config_field(std::move(life_config)) {}
};

struct NodePortDefinition : PortContract {
  std::string logical_name;

  NodePortDefinition() = default;
  NodePortDefinition(std::string name, std::string type, bool req = true,
                     std::string card = "1:1",
                     std::string provenance = "preserve",
                     std::string life = "request", std::string life_config = {})
      : PortContract(std::move(type), req, std::move(card),
                     std::move(provenance), std::move(life),
                     std::move(life_config)),
        logical_name(std::move(name)) {}
  const std::string& Name() const { return logical_name; }
};

struct BizPortDefinition : PortContract {
  std::string blackboard_key;

  BizPortDefinition() = default;
  BizPortDefinition(std::string name, std::string type, bool req = true,
                    std::string card = "1:1",
                    std::string provenance = "preserve",
                    std::string life = "request", std::string life_config = {})
      : PortContract(std::move(type), req, std::move(card),
                     std::move(provenance), std::move(life),
                     std::move(life_config)),
        blackboard_key(std::move(name)) {}
  const std::string& Name() const { return blackboard_key; }
};

template <typename T>
inline BizPortDefinition RequiredBizInput(const BlackboardKey<T>& key) {
  return {key.name, key.type_id, true};
}

template <typename T>
inline BizPortDefinition OptionalBizInput(const BlackboardKey<T>& key) {
  return {key.name, key.type_id, false};
}

template <typename T>
inline BizPortDefinition BizOutput(const BlackboardKey<T>& key) {
  return {key.name, key.type_id, true};
}

template <typename T>
inline NodePortDefinition RequiredInputPort(
    std::string logical_name, const BlackboardKey<T>& key_type,
    std::string cardinality = "1:1", std::string provenance = "preserve",
    std::string lifetime = "request", std::string lifetime_config_field = {}) {
  return NodePortDefinition{std::move(logical_name),
                            key_type.type_id,
                            true,
                            std::move(cardinality),
                            std::move(provenance),
                            std::move(lifetime),
                            std::move(lifetime_config_field)};
}

template <typename T>
inline NodePortDefinition RequiredInputPort(
    const BlackboardKey<T>& key, std::string cardinality = "1:1",
    std::string provenance = "preserve", std::string lifetime = "request",
    std::string lifetime_config_field = {}) {
  return RequiredInputPort(key.name, key, std::move(cardinality),
                           std::move(provenance), std::move(lifetime),
                           std::move(lifetime_config_field));
}

template <typename T>
inline NodePortDefinition OptionalInputPort(
    std::string logical_name, const BlackboardKey<T>& key_type,
    std::string cardinality = "1:1", std::string provenance = "preserve",
    std::string lifetime = "request", std::string lifetime_config_field = {}) {
  return NodePortDefinition{std::move(logical_name),
                            key_type.type_id,
                            false,
                            std::move(cardinality),
                            std::move(provenance),
                            std::move(lifetime),
                            std::move(lifetime_config_field)};
}

template <typename T>
inline NodePortDefinition OptionalInputPort(
    const BlackboardKey<T>& key, std::string cardinality = "1:1",
    std::string provenance = "preserve", std::string lifetime = "request",
    std::string lifetime_config_field = {}) {
  return OptionalInputPort(key.name, key, std::move(cardinality),
                           std::move(provenance), std::move(lifetime),
                           std::move(lifetime_config_field));
}

template <typename T>
inline NodePortDefinition OutputPort(std::string logical_name,
                                     const BlackboardKey<T>& key_type,
                                     std::string cardinality = "1:1",
                                     std::string provenance = "preserve",
                                     std::string lifetime = "request",
                                     std::string lifetime_config_field = {}) {
  return NodePortDefinition{std::move(logical_name),
                            key_type.type_id,
                            true,
                            std::move(cardinality),
                            std::move(provenance),
                            std::move(lifetime),
                            std::move(lifetime_config_field)};
}

template <typename T>
inline NodePortDefinition OutputPort(const BlackboardKey<T>& key,
                                     std::string cardinality = "1:1",
                                     std::string provenance = "preserve",
                                     std::string lifetime = "request",
                                     std::string lifetime_config_field = {}) {
  return OutputPort(key.name, key, std::move(cardinality),
                    std::move(provenance), std::move(lifetime),
                    std::move(lifetime_config_field));
}

}  // namespace llm_edgeflow

#pragma once

#include <string>
#include <utility>

#include "core/blackboard_key.h"

namespace llm_edgeflow {

struct FollowLifetime {
  std::string input;
  explicit FollowLifetime(std::string port) : input(std::move(port)) {}
};

// 与端口命名方式无关的共享流属性。
struct PortContract {
  std::string type_id;
  bool required = true;
  std::string cardinality = "1:1";
  std::string provenance_policy = "preserve";
  std::string lifetime = "request";
  std::string lifetime_from_input;

  PortContract() = default;
  PortContract(std::string type, bool req = true, std::string card = "1:1",
               std::string provenance = "preserve",
               std::string life = "request", std::string life_input = {})
      : type_id(std::move(type)),
        required(req),
        cardinality(std::move(card)),
        provenance_policy(std::move(provenance)),
        lifetime(std::move(life)),
        lifetime_from_input(std::move(life_input)) {}
};

struct NodePortDefinition : PortContract {
  std::string logical_name;

  NodePortDefinition() = default;
  NodePortDefinition(std::string name, std::string type, bool req = true,
                     std::string card = "1:1",
                     std::string provenance = "preserve",
                     std::string life = "request", std::string life_input = {})
      : PortContract(std::move(type), req, std::move(card),
                     std::move(provenance), std::move(life),
                     std::move(life_input)),
        logical_name(std::move(name)) {}
  const std::string& Name() const { return logical_name; }
};

struct IoPortDefinition : PortContract {
  std::string blackboard_key;
  std::string logical_name;
  std::string path;
  bool has_binding = false;

  IoPortDefinition() = default;
  IoPortDefinition(std::string name, std::string type, bool req = true,
                   std::string card = "1:1",
                   std::string provenance = "preserve",
                   std::string life = "request", std::string life_input = {})
      : PortContract(std::move(type), req, std::move(card),
                     std::move(provenance), std::move(life),
                     std::move(life_input)),
        blackboard_key(std::move(name)) {}
  const std::string& Name() const { return blackboard_key; }
};

template <typename T>
inline NodePortDefinition RequiredInputPort(
    std::string logical_name, const BlackboardKey<T>& key_type,
    std::string cardinality = "1:1", std::string provenance = "preserve",
    std::string lifetime = "request", std::string lifetime_from_input = {}) {
  return NodePortDefinition{
      std::move(logical_name),       key_type.type_id,      true,
      std::move(cardinality),        std::move(provenance), std::move(lifetime),
      std::move(lifetime_from_input)};
}

template <typename T>
inline NodePortDefinition RequiredInputPort(
    const BlackboardKey<T>& key, std::string cardinality = "1:1",
    std::string provenance = "preserve", std::string lifetime = "request",
    std::string lifetime_from_input = {}) {
  return RequiredInputPort(key.name, key, std::move(cardinality),
                           std::move(provenance), std::move(lifetime),
                           std::move(lifetime_from_input));
}

template <typename T>
inline NodePortDefinition OptionalInputPort(
    std::string logical_name, const BlackboardKey<T>& key_type,
    std::string cardinality = "1:1", std::string provenance = "preserve",
    std::string lifetime = "request", std::string lifetime_from_input = {}) {
  return NodePortDefinition{
      std::move(logical_name),       key_type.type_id,      false,
      std::move(cardinality),        std::move(provenance), std::move(lifetime),
      std::move(lifetime_from_input)};
}

template <typename T>
inline NodePortDefinition OptionalInputPort(
    const BlackboardKey<T>& key, std::string cardinality = "1:1",
    std::string provenance = "preserve", std::string lifetime = "request",
    std::string lifetime_from_input = {}) {
  return OptionalInputPort(key.name, key, std::move(cardinality),
                           std::move(provenance), std::move(lifetime),
                           std::move(lifetime_from_input));
}

template <typename T>
inline NodePortDefinition OutputPort(std::string logical_name,
                                     const BlackboardKey<T>& key_type,
                                     std::string cardinality = "1:1",
                                     std::string provenance = "preserve",
                                     std::string lifetime = "request",
                                     std::string lifetime_from_input = {}) {
  return NodePortDefinition{
      std::move(logical_name),       key_type.type_id,      true,
      std::move(cardinality),        std::move(provenance), std::move(lifetime),
      std::move(lifetime_from_input)};
}

template <typename T>
inline NodePortDefinition OutputPort(const BlackboardKey<T>& key,
                                     std::string cardinality = "1:1",
                                     std::string provenance = "preserve",
                                     std::string lifetime = "request",
                                     std::string lifetime_from_input = {}) {
  return OutputPort(key.name, key, std::move(cardinality),
                    std::move(provenance), std::move(lifetime),
                    std::move(lifetime_from_input));
}

}  // namespace llm_edgeflow

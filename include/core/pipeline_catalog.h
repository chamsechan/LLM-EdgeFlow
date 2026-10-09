#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "contracts/config_schema.h"
#include "core/node_definition.h"
#include "engine/inference_definition.h"

namespace llm_edgeflow {

struct PipelineCatalogSnapshot {
  std::vector<NodeDefinition> nodes;
  bool node_registry_has_conflict = false;
  std::vector<std::string> node_registry_errors;

  const NodeDefinition* FindNode(const std::string& node_type) const;
};

class PipelineCatalog {
 public:
  static PipelineCatalogSnapshot Snapshot();
  static std::vector<NodeDefinition> Nodes();
  static std::vector<ModelDefinition> Models();
  static std::vector<BackendDefinition> Backends();

  static std::optional<NodeDefinition> FindNode(const std::string& node_type);
  static std::optional<ModelDefinition> FindModel(
      const std::string& model_type);
  static std::optional<BackendDefinition> FindBackend(
      const std::string& backend_type);
  static nlohmann::json ToJson(const PipelineCatalogSnapshot& snapshot);
  static nlohmann::json ToJson();
  static nlohmann::json PortToJson(const std::string& key,
                                   const PortContract& port);
  static nlohmann::json NodeToJson(const NodeDefinition& definition);
  static nlohmann::json ModelToJson(const ModelDefinition& definition);
  static nlohmann::json BackendToJson(const BackendDefinition& definition);
};

}  // namespace llm_edgeflow

#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "contracts/config_schema.h"
#include "core/biz_definition.h"
#include "core/node_definition.h"
#include "engine/inference_definition.h"

namespace llm_edgeflow {

namespace test_support {
class RegistryTestAccess;
}

struct PipelineCatalogSnapshot {
  std::vector<NodeDefinition> nodes;
  std::vector<BizDefinition> bizs;
  bool node_registry_has_conflict = false;
  std::vector<std::string> node_registry_errors;

  const NodeDefinition* FindNode(const std::string& node_type) const;
  const BizDefinition* FindBiz(const std::string& biz_name) const;
};

class PipelineCatalog {
 public:
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

  static nlohmann::json ToJson(const PipelineCatalogSnapshot& snapshot,
                               const std::string& biz_filter = std::string());
  static nlohmann::json ToJson(const std::string& biz_filter = std::string());
  static nlohmann::json PortToJson(const std::string& key,
                                   const PortContract& port);
  static nlohmann::json NodeToJson(const NodeDefinition& definition);
  static nlohmann::json ModelToJson(const ModelDefinition& definition);
  static nlohmann::json BackendToJson(const BackendDefinition& definition);

 private:
  friend class test_support::RegistryTestAccess;
  static void ResetBizsForTesting();
};

}  // namespace llm_edgeflow

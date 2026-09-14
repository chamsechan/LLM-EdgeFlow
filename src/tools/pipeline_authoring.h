#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace llm_edgeflow {

struct AuthoringChange {
  std::string action;
  std::string node_id;
  std::string port;
  std::vector<std::string> affected_nodes;
  std::string description;

  nlohmann::json ToJson() const;
};

struct AuthoringResult {
  int schema_version = 1;
  bool ok = false;
  std::optional<nlohmann::json> pipeline;
  std::vector<AuthoringChange> changes;
  nlohmann::json validation = nlohmann::json::object();
  std::vector<std::string> diagnostics;
  std::optional<size_t> failed_operation_index;

  nlohmann::json ToJson() const;
};

struct FixDepsResult {
  int schema_version = 1;
  bool ok = false;
  std::string target_file;
  bool written = false;
  std::vector<AuthoringChange> changes;
  nlohmann::json validation = nlohmann::json::object();
  std::vector<std::string> diagnostics;

  nlohmann::json ToJson() const;
};

class PipelineAuthoring {
 public:
  // Applies a single authoring request containing {schema_version, pipeline,
  // operation/operations, require_valid}.
  static AuthoringResult ApplyRequest(const nlohmann::json& request);

  // Applies one authoring operation to an in-memory pipeline document.
  static bool ApplyOperation(nlohmann::json* pipeline,
                             const nlohmann::json& operation,
                             std::vector<AuthoringChange>* changes,
                             std::string* error);

  // Analyzes missing producer dependencies, previews or applies fixes in-place.
  static FixDepsResult FixDeps(const std::string& file_path, bool in_place);

  // Collects all occupied blackboard keys in document (explicit, default,
  // placeholders, ingress, egress).
  static std::unordered_set<std::string> GetOccupiedKeys(
      const nlohmann::json& pipeline);

  // Allocates a unique key based on base name without colliding with occupied
  // keys.
  static std::string AllocateKey(
      const std::string& base, const std::unordered_set<std::string>& occupied);

  // Checks whether potential_ancestor is a direct or transitive dependency of
  // node_id.
  static bool IsAncestor(
      const std::string& potential_ancestor, const std::string& node_id,
      const std::unordered_map<std::string, std::vector<std::string>>&
          dep_graph);

  // Builds a depends_on adjacency graph mapping node_id to its listed
  // dependencies.
  static std::unordered_map<std::string, std::vector<std::string>>
  BuildDependencyGraph(const nlohmann::json& pipeline);
};

}  // namespace llm_edgeflow

#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace alg_framework {

struct ValidationDiagnostic {
  std::string code;
  std::string path;
  std::string message;
  std::string severity = "error";
  std::string node_id;
  std::string port;
  std::vector<std::string> related_nodes;
  std::vector<std::string> suggestions;
};

struct ValidationReport {
  bool ok = false;
  std::vector<ValidationDiagnostic> diagnostics;
  std::vector<std::string> topological_order;
  std::vector<std::vector<std::string>> topological_layers;

  nlohmann::json ToJson() const;
};

class PipelineValidator {
 public:
  static ValidationReport Validate(const nlohmann::json& root);

  /** Upgrade an implicit sequential pipeline to explicit DAG form. */
  static bool NormalizeExplicitDag(const nlohmann::json& root,
                                   nlohmann::json* output,
                                   ValidationDiagnostic* diagnostic = nullptr);
};

}  // namespace alg_framework

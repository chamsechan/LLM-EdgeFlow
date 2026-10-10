#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace llm_edgeflow {

struct AuthoringChange {
  std::string action;
  std::string node_name;
  std::string port;
  std::vector<std::string> affected_nodes;
  std::string description;

  nlohmann::json ToJson() const;
};

struct AuthoringResult {
  bool ok = false;
  std::optional<nlohmann::json> pipeline;
  std::vector<AuthoringChange> changes;
  nlohmann::json validation = nlohmann::json::object();
  std::vector<std::string> diagnostics;
  std::optional<size_t> failed_operation_index;

  nlohmann::json ToJson() const;
};

class PipelineAuthoring {
 public:
  // 应用单个编辑请求，包含 {pipeline, operation/operations, require_valid}。
  static AuthoringResult ApplyRequest(const nlohmann::json& request);

  // 将一个编辑操作应用到内存中的 Pipeline 文档。
  static bool ApplyOperation(nlohmann::json* pipeline,
                             const nlohmann::json& operation,
                             std::vector<AuthoringChange>* changes,
                             std::string* error);
};

}  // namespace llm_edgeflow

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

  // 收集文档中所有已占用的 Blackboard 键
  // (显式映射、默认输出、ingress、egress)。
  static std::unordered_set<std::string> GetOccupiedKeys(
      const nlohmann::json& pipeline);

  // 基于 base 名称分配一个不与已占用键冲突的唯一键。
  static std::string AllocateKey(
      const std::string& base, const std::unordered_set<std::string>& occupied);

  // 检查 potential_ancestor 是否为 node_id 的直接或传递依赖。
  static bool IsAncestor(
      const std::string& potential_ancestor, const std::string& node_id,
      const std::unordered_map<std::string, std::vector<std::string>>&
          dep_graph);

  // 构建 depends_on 邻接图：node_id -> 其列出的依赖。
  static std::unordered_map<std::string, std::vector<std::string>>
  BuildDependencyGraph(const nlohmann::json& pipeline);
};

}  // namespace llm_edgeflow

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
  /**
   * @brief 在文档副本上应用单个或多个编辑操作，返回编辑结果与完整校验报告。
   * 操作失败时不交付文档；require_valid 为 true 时还要求最终语义校验通过。
   * 只修改返回的内存文档，持久化由调用方负责。
   */
  static AuthoringResult ApplyRequest(const nlohmann::json& request);
};

}  // namespace llm_edgeflow

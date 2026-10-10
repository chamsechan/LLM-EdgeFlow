#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/diagnostic_code.h"
#include "core/pipeline_config.h"
#include "core/port_definition.h"
#include "core/validated_node_plan.h"
#include "engine/inference_definition.h"

namespace llm_edgeflow {

struct ValidationDiagnostic {
  DiagnosticCode code = DiagnosticCode::kOk;
  std::string path;
  std::string message;
  std::string severity = "error";
  std::string node_name;
  std::string port;
  std::vector<std::string> related_nodes;
  std::vector<std::string> suggestions;
  // 校验确立的中性事实，含推断出的 item 形状。
  // 工具可据此解释，无需重复 Validator 规则。
  nlohmann::json facts = nlohmann::json::object();

  nlohmann::json ToJson() const;
};

struct ValidationReport {
  bool ok = false;
  std::vector<ValidationDiagnostic> diagnostics;
  std::vector<std::string> topological_order;
  std::vector<std::vector<std::string>> topological_layers;

  nlohmann::json ToJson() const;
};

struct ValidatedModelPlan {
  std::string model_name;
  std::string model_type;
  std::string impl_name;
  std::string backend_type;
  std::string model_file;
  nlohmann::json model_params = nlohmann::json::object();
  nlohmann::json backend_params = nlohmann::json::object();
  ExecutionProtocol protocol = ExecutionProtocol::kTensorGraph;
  InferenceConcurrency effective_concurrency =
      InferenceConcurrency::kSerialized;
  size_t source_index = 0;
};

struct ValidatedPipelinePlan {
  ParsedPipelineConfig config;
  std::vector<ValidatedModelPlan> models;
  std::vector<IoPortDefinition> input_ports;  // 被引用的输入项输出。
  std::unordered_map<std::string, ValidatedNodePlan> node_plans;
  ValidationReport report;
};

struct PipelineIoBoundary {
  std::vector<IoPortDefinition> input_published_ports;
  std::vector<IoPortDefinition> output_consumed_ports;
};

class PipelineValidator {
 public:
  static ValidatedPipelinePlan ValidateAndPlan(
      const nlohmann::json& root, const PipelineIoBoundary& io_boundary);

  // Consumes the successful result of ParsePipelineConfig without reparsing
  // JSON.
  static ValidatedPipelinePlan ValidateParsedAndPlan(
      ParsedPipelineConfig config, const PipelineIoBoundary& io_boundary);

  static ValidationReport Validate(const nlohmann::json& root,
                                   const PipelineIoBoundary& io_boundary);
};

/**
 * @brief 按 Schema Definition 校验用户配置对象并补齐默认值
 * @param schema 字段定义列表
 * @param input 原始用户传入的 JSON 配置
 * @param normalized 输出归一化后的新 JSON 配置 (注入默认值)
 * @param diagnostics 可选的诊断错误收集列表
 * @param base_pointer JSON Pointer 基础前缀 (如 "/models/0/params")
 * @return true 校验通过且成功归一化，false 校验失败
 */
bool ValidateAndNormalizeConfig(
    const std::vector<ConfigFieldDefinition>& schema,
    const nlohmann::json& input, nlohmann::json* normalized,
    std::vector<ValidationDiagnostic>* diagnostics,
    const std::string& base_pointer = "");

}  // namespace llm_edgeflow

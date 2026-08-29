#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/pipeline_config.h"
#include "engine/inference_definition.h"

namespace alg_framework {

enum class ValidationPolicy {
  kStrict,
  kPrivateExtensionCompatible,
};

enum class DiagnosticCode {
  kOk,
  kJsonParse,
  kConfigFileOpen,
  kRootType,
  kUnknownField,
  kMissingField,
  kFieldType,
  kFieldRange,
  kInvalidCombination,
  kDuplicateModelId,
  kDuplicateNodeId,
  kUnknownBiz,
  kUnknownBusiness = kUnknownBiz,
  kUnknownNodeType,
  kUnknownEngineType,
  kUnknownModelType,
  kUnknownBackend,
  kModelCapabilityMismatch,
  kBackendProtocolMismatch,
  kUnknownModelConfigField,
  kUnknownBackendConfigField,
  kInvalidDependency,
  kDuplicateDependency,
  kDagCycle,
  kRegistryConflict,
  kUnknownConfigField,
  kMissingConfigField,
  kConfigFieldType,
  kConfigFieldRange,
  kConfigFieldEnum,
  kUnknownModelReference,
  kNodeBizMismatch,
  kNodeBusinessMismatch = kNodeBizMismatch,
  kMissingInputProducer,
  kDuplicatePortProducer,
  kMissingBizOutput,
  kMissingBusinessOutput = kMissingBizOutput,
  kNodeNotParallelSafe,
  kParallelWriteConflict,
  kSerializedEngineConcurrency,
  kPortCardinalityMismatch,
  kPortProvenanceMismatch,
  kPortLifetimeMismatch,
  kInternalException,
};

const char* DiagnosticCodeName(DiagnosticCode code) noexcept;

struct ValidationDiagnostic {
  DiagnosticCode code = DiagnosticCode::kOk;
  std::string path;
  std::string message;
  std::string severity = "error";
  std::string node_id;
  std::string port;
  std::vector<std::string> related_nodes;
  std::vector<std::string> suggestions;

  nlohmann::json ToJson() const;
};

struct ValidationReport {
  bool ok = false;
  std::vector<ValidationDiagnostic> diagnostics;
  std::vector<std::string> topological_order;
  std::vector<std::vector<std::string>> topological_layers;

  nlohmann::json ToJson() const;
};

enum class PortDirection { kInput, kOutput };

struct ResolvedPortBinding {
  std::string logical_name;
  std::string blackboard_key;
  std::string type_id;
  std::string cardinality;
  std::string provenance_policy;
  std::string lifetime;
  PortDirection direction = PortDirection::kInput;
};

struct ValidatedModelPlan {
  std::string model_id;
  std::string capability;
  std::string model_type;
  std::string backend;
  std::string resolved_model_path;
  nlohmann::json normalized_model_config = nlohmann::json::object();
  nlohmann::json normalized_backend_config = nlohmann::json::object();
  ExecutionProtocol protocol = ExecutionProtocol::kTensorGraph;
  InferenceConcurrency effective_concurrency =
      InferenceConcurrency::kSerialized;
  size_t source_index = 0;
};

struct ValidatedNodePlan {
  ParsedNodeConfig node;
  nlohmann::json normalized_config;
  std::vector<ResolvedPortBinding> ports;

  std::string FindPortKey(const std::string& logical_name,
                          PortDirection dir = PortDirection::kInput) const {
    for (const auto& p : ports) {
      if (p.logical_name == logical_name && p.direction == dir) {
        return p.blackboard_key;
      }
    }
    return {};
  }

  const ResolvedPortBinding* FindPort(
      const std::string& logical_name,
      PortDirection dir = PortDirection::kInput) const {
    for (const auto& p : ports) {
      if (p.logical_name == logical_name && p.direction == dir) return &p;
    }
    return nullptr;
  }
};

struct ValidatedPipelinePlan {
  ParsedPipelineConfig config;
  std::vector<ValidatedModelPlan> models;
  std::vector<std::string> topological_order;
  std::vector<std::vector<std::string>> topological_layers;
  std::unordered_map<std::string, ValidatedNodePlan> node_plans;
  ValidationReport report;
};

class PipelineValidator {
 public:
  static ValidatedPipelinePlan ValidateAndPlan(
      const nlohmann::json& root,
      ValidationPolicy policy = ValidationPolicy::kStrict,
      const std::string& model_root_dir = "");

  static ValidationReport Validate(
      const nlohmann::json& root,
      ValidationPolicy policy = ValidationPolicy::kStrict,
      const std::string& model_root_dir = "");

  /** Upgrade an implicit sequential pipeline to explicit DAG form. */
  static bool NormalizeExplicitDag(const nlohmann::json& root,
                                   nlohmann::json* output,
                                   ValidationDiagnostic* diagnostic = nullptr);
};

/**
 * @brief 按 Schema Definition 校验用户配置对象并补齐默认值
 * @param schema 字段定义列表
 * @param input 原始用户传入的 JSON 配置
 * @param normalized 输出归一化后的新 JSON 配置 (注入默认值)
 * @param diagnostics 可选的诊断错误收集列表
 * @param base_pointer JSON Pointer 基础前缀 (如 "/models/0/model_config")
 * @param unknown_field_code 未知字段诊断码 (缺省为 kUnknownConfigField)
 * @return true 校验通过且成功归一化，false 校验失败
 */
bool ValidateAndNormalizeConfig(
    const std::vector<ConfigFieldDefinition>& schema,
    const nlohmann::json& input, nlohmann::json* normalized,
    std::vector<ValidationDiagnostic>* diagnostics,
    const std::string& base_pointer = "",
    DiagnosticCode unknown_field_code = DiagnosticCode::kUnknownConfigField);

}  // namespace alg_framework

#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/pipeline_config.h"

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
  kModelCapabilityMismatch,
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
  std::vector<std::string> topological_order;
  std::vector<std::vector<std::string>> topological_layers;
  std::unordered_map<std::string, ValidatedNodePlan> node_plans;
  ValidationReport report;
};

class PipelineValidator {
 public:
  static ValidatedPipelinePlan ValidateAndPlan(
      const nlohmann::json& root,
      ValidationPolicy policy = ValidationPolicy::kStrict);

  static ValidationReport Validate(
      const nlohmann::json& root,
      ValidationPolicy policy = ValidationPolicy::kStrict);

  /** Upgrade an implicit sequential pipeline to explicit DAG form. */
  static bool NormalizeExplicitDag(const nlohmann::json& root,
                                   nlohmann::json* output,
                                   ValidationDiagnostic* diagnostic = nullptr);
};

}  // namespace alg_framework

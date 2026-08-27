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

struct ValidatedPipelinePlan {
  ParsedPipelineConfig config;
  std::vector<std::string> topological_order;
  std::vector<std::vector<std::string>> topological_layers;
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

#include "pipeline_document_validation.h"

#include <utility>

#include "adapter/deployment_preparation.h"
#include "core/pipeline_validator.h"

namespace llm_edgeflow {

DocumentValidationResult ValidatePipelineDocument(
    const nlohmann::json& document, DocumentValidationMode mode) {
  DocumentValidationResult result;

  PreparedDeployment prepared;
  {
    DeploymentPrepareOptions options;

    options.path_mode = DeploymentPathMode::kLexicalOnly;
    options.model_root_dir = "";

    DeploymentDiagnostic diag;
    if (!PrepareDeploymentDocument(document, options, &prepared, &diag)) {
      result.ok = false;
      result.core_report = std::nullopt;

      nlohmann::json diag_item = {{"code", diag.code},
                                  {"path", diag.path},
                                  {"message", diag.message},
                                  {"severity", "error"}};
      nlohmann::json resp = {
          {"schema_version", 1},
          {"ok", false},
          {"diagnostics", nlohmann::json::array({diag_item})}};
      if (mode == DocumentValidationMode::kPlan) {
        resp["plan"] = {{"layers", nlohmann::json::array()},
                        {"topological_order", nlohmann::json::array()}};
      }
      result.response = std::move(resp);
      return result;
    }
  }
  auto report =
      mode == DocumentValidationMode::kExplain
          ? PipelineValidator::Explain(prepared.neutral_pipeline_json,
                                       &prepared.io_boundary)
          : PipelineValidator::Validate(prepared.neutral_pipeline_json,
                                        &prepared.io_boundary);
  ProjectDeploymentDiagnostics(&report);
  result.ok = report.ok;
  result.response = report.ToJson();
  if (mode == DocumentValidationMode::kPlan && report.ok)
    result.response.erase("diagnostics");
  result.core_report = std::move(report);
  return result;
}

}  // namespace llm_edgeflow

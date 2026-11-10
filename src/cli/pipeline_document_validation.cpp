#include "pipeline_document_validation.h"

#include <utility>

#include "adapter/deployment_preparation.h"
#include "core/pipeline_validator.h"

namespace llm_edgeflow {

DocumentValidationResult ValidatePipelineDocument(
    const nlohmann::json& document, DocumentValidationMode mode,
    const std::string& pipeline_dir) {
  DocumentValidationResult result;

  PreparedDeployment prepared;
  {
    DeploymentPrepareOptions options;

    options.pipeline_dir = pipeline_dir;

    DeploymentDiagnostic diag;
    if (!PrepareDeploymentDocument(document, options, &prepared, &diag)) {
      result.ok = false;
      result.core_report = std::nullopt;

      nlohmann::json diag_item = {{"code", ErrorCodeName(diag.code)},
                                  {"path", diag.path},
                                  {"message", diag.message}};
      nlohmann::json resp = {
          {"ok", false}, {"diagnostics", nlohmann::json::array({diag_item})}};
      if (mode == DocumentValidationMode::kPlan) {
        resp["plan"] = {{"layers", nlohmann::json::array()},
                        {"topological_order", nlohmann::json::array()}};
      }
      result.response = std::move(resp);
      return result;
    }
  }
  result.core_report =
      PipelineValidator::ValidateParsedAndPlan(
          std::move(prepared.parsed_pipeline), prepared.io_boundary)
          .report;
  const auto& report = *result.core_report;
  result.ok = report.ok;
  result.response = report.ToJson();
  if (mode == DocumentValidationMode::kPlan && report.ok)
    result.response.erase("diagnostics");
  return result;
}

}  // namespace llm_edgeflow

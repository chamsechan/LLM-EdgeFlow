#include "pipeline_document_validation.h"

#include <utility>

#include "adapter/deployment_preparation.h"
#include "core/pipeline_validator.h"
#include "pipeline_remediation.h"

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

      nlohmann::json diag_item = {{"code", diag.code},
                                  {"path", diag.path},
                                  {"message", diag.message},
                                  {"severity", "error"}};
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
  auto report = ValidateWithRemediation(prepared.neutral_pipeline_json,
                                        prepared.io_boundary);
  if (mode == DocumentValidationMode::kExplain) {
    report =
        ExplainPipeline(document, std::move(report), [&](const auto& patched) {
          auto validation = ValidatePipelineDocument(
              patched, DocumentValidationMode::kValidate, pipeline_dir);
          if (validation.core_report) return std::move(*validation.core_report);
          ValidationReport failed;
          ValidationDiagnostic diagnostic;
          diagnostic.code = DiagnosticCode::kInvalidCombination;
          const auto& error = validation.response.at("diagnostics").at(0);
          diagnostic.path = error.at("path").template get<std::string>();
          diagnostic.message = error.at("message").template get<std::string>();
          failed.diagnostics.push_back(std::move(diagnostic));
          return failed;
        });
  }
  result.ok = report.ok;
  result.response = report.ToJson();
  if (mode == DocumentValidationMode::kPlan && report.ok)
    result.response.erase("diagnostics");
  result.core_report = std::move(report);
  return result;
}

}  // namespace llm_edgeflow

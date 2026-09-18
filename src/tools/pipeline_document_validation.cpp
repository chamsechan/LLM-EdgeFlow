#include "pipeline_document_validation.h"

#include <utility>

#include "adapter/deployment_preparation.h"
#include "core/pipeline_validator.h"

namespace llm_edgeflow {

DocumentValidationResult ValidatePipelineDocument(
    const nlohmann::json& document, DocumentValidationMode mode) {
  DocumentValidationResult result;

  if (document.is_object() && document.contains("deployment")) {
    DeploymentPrepareOptions options;
    options.transport = "operator";
    options.path_mode = DeploymentPathMode::kLexicalOnly;
    options.model_root_dir = "";

    PreparedDeployment prepared;
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

    if (mode == DocumentValidationMode::kValidate) {
      auto report = PipelineValidator::Validate(prepared.neutral_pipeline_json,
                                                ValidationPolicy::kStrict,
                                                &prepared.io_boundary);
      ProjectModelPathDiagnostics(prepared, &report);
      result.ok = report.ok;
      result.response = report.ToJson();
      result.core_report = std::move(report);
    } else if (mode == DocumentValidationMode::kExplain) {
      auto report = PipelineValidator::Explain(prepared.neutral_pipeline_json,
                                               ValidationPolicy::kStrict,
                                               &prepared.io_boundary);
      ProjectModelPathDiagnostics(prepared, &report);
      result.ok = report.ok;
      result.response = report.ToJson();
      result.core_report = std::move(report);
    } else {
      auto planned = PipelineValidator::ValidateAndPlan(
          prepared.neutral_pipeline_json, ValidationPolicy::kStrict,
          &prepared.io_boundary);
      ProjectModelPathDiagnostics(prepared, &planned.report);
      result.ok = planned.report.ok;
      auto resp = planned.report.ToJson();
      if (planned.report.ok) {
        resp.erase("diagnostics");
      }
      result.response = std::move(resp);
      result.core_report = std::move(planned.report);
    }
    return result;
  }

  // 不含 deployment，或根不是对象：原样交给 Core
  if (mode == DocumentValidationMode::kValidate) {
    auto report =
        PipelineValidator::Validate(document, ValidationPolicy::kStrict);
    result.ok = report.ok;
    result.response = report.ToJson();
    result.core_report = std::move(report);
  } else if (mode == DocumentValidationMode::kExplain) {
    auto report =
        PipelineValidator::Explain(document, ValidationPolicy::kStrict);
    result.ok = report.ok;
    result.response = report.ToJson();
    result.core_report = std::move(report);
  } else {
    auto planned =
        PipelineValidator::ValidateAndPlan(document, ValidationPolicy::kStrict);
    result.ok = planned.report.ok;
    auto resp = planned.report.ToJson();
    if (planned.report.ok) {
      resp.erase("diagnostics");
    }
    result.response = std::move(resp);
    result.core_report = std::move(planned.report);
  }
  return result;
}

}  // namespace llm_edgeflow

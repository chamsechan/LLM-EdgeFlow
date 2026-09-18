#include "adapter/io_binding_resolver.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "adapter/deployment_preparation.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "adapter/pipeline_document.h"
#include "core/diagnostic_code.h"

namespace llm_edgeflow {

namespace fs = std::filesystem;

int IoBindingResolver::ResolveFromFile(
    const std::string& config_path, const std::string& transport,
    const std::string& model_root_dir,
    std::unique_ptr<ValidatedIoPlan>* out_plan, std::string* out_error,
    DeploymentDiagnostic* out_diagnostic) {
  if (out_diagnostic) out_diagnostic->Clear();

  DeploymentIoConfig config;
  std::string err;
  if (!DeploymentIoConfig::ReadFromFile(config_path, transport, &config, &err,
                                        out_diagnostic)) {
    if (out_error) *out_error = err;
    return -2;
  }
  return ResolveFromConfig(config, transport, model_root_dir, out_plan,
                           out_error, out_diagnostic);
}

int IoBindingResolver::ResolveFromConfig(
    const DeploymentIoConfig& config, const std::string& transport,
    const std::string& model_root_dir,
    std::unique_ptr<ValidatedIoPlan>* out_plan, std::string* out_error,
    DeploymentDiagnostic* out_diagnostic) {
  if (out_diagnostic) out_diagnostic->Clear();

  if (!out_plan) {
    if (out_error) *out_error = "Null out_plan pointer";
    if (out_diagnostic) {
      out_diagnostic->code = "DEPLOYMENT_ERROR";
      out_diagnostic->path = "/";
      out_diagnostic->message = "Null out_plan pointer";
      out_diagnostic->legacy_status = -1;
    }
    return -1;
  }
  *out_plan = nullptr;

  std::ifstream pipe_ifs(config.resolved_pipe_path);
  if (!pipe_ifs.is_open()) {
    std::string msg =
        "Failed to open pipeline file: " + config.resolved_pipe_path;
    if (out_error) *out_error = msg;
    if (out_diagnostic) {
      out_diagnostic->code = "CONFIG_FILE_OPEN";
      out_diagnostic->path = "/";
      out_diagnostic->message = msg;
      out_diagnostic->legacy_status = -2;
    }
    return -2;
  }
  nlohmann::json raw_pipe_json;
  try {
    pipe_ifs >> raw_pipe_json;
  } catch (const std::exception& e) {
    std::string msg =
        "JSON parse exception in pipeline file: " + config.resolved_pipe_path +
        ": " + e.what();
    if (out_error) *out_error = msg;
    if (out_diagnostic) {
      out_diagnostic->code = "JSON_PARSE";
      out_diagnostic->path = "/";
      out_diagnostic->message = msg;
      out_diagnostic->legacy_status = -2;
    }
    return -2;
  }

  return ResolveFromPipelineJson(raw_pipe_json, transport, model_root_dir,
                                 out_plan, out_error, out_diagnostic);
}

int IoBindingResolver::ResolveFromPipelineJson(
    const nlohmann::json& pipeline_json, const std::string& transport,
    const std::string& model_root_dir,
    std::unique_ptr<ValidatedIoPlan>* out_plan, std::string* out_error,
    DeploymentDiagnostic* out_diagnostic) {
  if (out_diagnostic) out_diagnostic->Clear();

  if (!out_plan) {
    if (out_error) *out_error = "Null out_plan pointer";
    if (out_diagnostic) {
      out_diagnostic->code = "DEPLOYMENT_ERROR";
      out_diagnostic->path = "/";
      out_diagnostic->message = "Null out_plan pointer";
      out_diagnostic->legacy_status = -1;
    }
    return -1;
  }
  *out_plan = nullptr;

  DeploymentPrepareOptions options;
  options.transport = transport;
  options.path_mode = model_root_dir.empty() ? DeploymentPathMode::kLexicalOnly
                                             : DeploymentPathMode::kUnderRoot;
  options.model_root_dir = model_root_dir;

  PreparedDeployment prepared;
  DeploymentDiagnostic prep_diag;
  if (!PrepareDeploymentDocument(pipeline_json, options, &prepared,
                                 &prep_diag)) {
    if (out_error) *out_error = prep_diag.message;
    if (out_diagnostic) *out_diagnostic = prep_diag;
    return prep_diag.legacy_status;
  }

  // 默认深度下的句柄池载荷总预算校验 (RFC-0062 §2.5, §2.6)
  size_t total_handle_pool_bytes = 0;
  for (const auto& [slot_name, pool_spec] : prepared.output_specs) {
    const auto* output_binding =
        OperatorValueTypeRegistry::Instance().GetOutputBinding(
            pool_spec.type, pool_spec.allocator);
    if (!output_binding || output_binding->direction != IoDirection::kOutput) {
      std::string msg =
          "Missing output value binding for suffix '" + pool_spec.type + "'";
      if (out_error) *out_error = msg;
      if (out_diagnostic) {
        out_diagnostic->code = "INVALID_OUTPUT_ALLOCATION";
        out_diagnostic->path =
            "/deployment/io/output_allocations/" + EscapeJsonPointer(slot_name);
        out_diagnostic->message = msg;
        out_diagnostic->legacy_status = -2;
      }
      return -2;
    }
    size_t slot_pool_bytes = 0;
    std::string budget_err;
    if (!ComputeOutputPoolPayloadBytes(*output_binding, pool_spec,
                                       kDefaultOutputPoolDepth,
                                       &slot_pool_bytes, &budget_err)) {
      std::string msg = "Output pool budget calculation failed: " + budget_err;
      if (out_error) *out_error = msg;
      if (out_diagnostic) {
        out_diagnostic->code = "INVALID_OUTPUT_ALLOCATION";
        out_diagnostic->path =
            "/deployment/io/output_allocations/" + EscapeJsonPointer(slot_name);
        out_diagnostic->message = msg;
        out_diagnostic->legacy_status = -2;
      }
      return -2;
    }
    if (!CheckedAdd(total_handle_pool_bytes, slot_pool_bytes,
                    &total_handle_pool_bytes)) {
      std::string msg = "Handle pool budget addition overflowed";
      if (out_error) *out_error = msg;
      if (out_diagnostic) {
        out_diagnostic->code = "INVALID_OUTPUT_ALLOCATION";
        out_diagnostic->path = "/deployment/io/output_allocations";
        out_diagnostic->message = msg;
        out_diagnostic->legacy_status = -2;
      }
      return -2;
    }
  }
  if (total_handle_pool_bytes > kMaxHandlePoolPayloadBytes) {
    std::string msg = "Total output pool payload (" +
                      std::to_string(total_handle_pool_bytes) +
                      " bytes) exceeds per-handle payload budget (" +
                      std::to_string(kMaxHandlePoolPayloadBytes) + " bytes)";
    if (out_error) *out_error = msg;
    if (out_diagnostic) {
      out_diagnostic->code = "INVALID_OUTPUT_ALLOCATION";
      out_diagnostic->path = "/deployment/io/output_allocations";
      out_diagnostic->message = msg;
      out_diagnostic->legacy_status = -2;
    }
    return -2;
  }

  // 调用 PipelineValidator 进行统一中性计划验证 (Core 校验
  // neutral_pipeline_json)
  auto plan = std::make_unique<ValidatedPipelinePlan>(
      PipelineValidator::ValidateAndPlan(prepared.neutral_pipeline_json,
                                         ValidationPolicy::kStrict,
                                         &prepared.io_boundary));

  ProjectModelPathDiagnostics(prepared, &plan->report);

  if (!plan->report.ok) {
    if (!plan->report.diagnostics.empty()) {
      const auto& d = plan->report.diagnostics.front();
      std::string msg =
          "Validation failed: " + std::string(DiagnosticCodeName(d.code)) +
          " at " + d.path + ": " + d.message;
      if (out_error) *out_error = msg;
      if (out_diagnostic) {
        out_diagnostic->code = DiagnosticCodeName(d.code);
        out_diagnostic->path = d.path;
        out_diagnostic->message = d.message;
        out_diagnostic->legacy_status = -3;
        out_diagnostic->pipeline_diagnostic =
            PipelineDiagnostic{d.code, d.path, d.message};
      }
    } else {
      std::string msg = "Validation failed without diagnostics";
      if (out_error) *out_error = msg;
      if (out_diagnostic) {
        out_diagnostic->code = "VALIDATION_FAILED";
        out_diagnostic->path = "/";
        out_diagnostic->message = msg;
        out_diagnostic->legacy_status = -3;
      }
    }
    return -3;
  }

  // 组装不可变接入计划
  auto io_plan = std::make_unique<ValidatedIoPlan>();
  io_plan->binding = std::move(prepared.binding);
  io_plan->input_converter = prepared.input_converter;
  io_plan->output_converter = prepared.output_converter;
  io_plan->input_port_bindings = std::move(prepared.input_port_bindings);
  io_plan->output_port_bindings = std::move(prepared.output_port_bindings);
  io_plan->effective_max_batch_size = prepared.effective_max_batch_size;
  io_plan->operator_output_specs = std::move(prepared.output_specs);
  io_plan->operator_output_parameter_texts =
      std::move(prepared.output_parameter_texts);
  io_plan->overridden_model_ids = std::move(prepared.overridden_model_ids);
  io_plan->resolved_pipeline_json = std::move(prepared.neutral_pipeline_json);
  io_plan->pipeline_plan = std::move(plan);

  *out_plan = std::move(io_plan);
  return 0;
}

}  // namespace llm_edgeflow

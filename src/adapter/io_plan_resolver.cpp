#include "adapter/io_plan_resolver.h"

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

int IoPlanResolver::ResolveFromFile(const std::string& config_path,
                                    const std::string& model_root_dir,
                                    std::unique_ptr<ValidatedIoPlan>* out_plan,
                                    std::string* out_error,
                                    DeploymentDiagnostic* out_diagnostic,
                                    uint32_t output_pool_depth) {
  if (out_diagnostic) out_diagnostic->Clear();

  DeploymentIoConfig config;
  std::string err;
  if (!DeploymentIoConfig::ReadFromFile(config_path, &config, &err,
                                        out_diagnostic)) {
    if (out_error) *out_error = err;
    return -2;
  }
  return ResolveFromConfig(config, model_root_dir, out_plan, out_error,
                           out_diagnostic, output_pool_depth);
}

int IoPlanResolver::ResolveFromConfig(
    const DeploymentIoConfig& config, const std::string& model_root_dir,
    std::unique_ptr<ValidatedIoPlan>* out_plan, std::string* out_error,
    DeploymentDiagnostic* out_diagnostic, uint32_t output_pool_depth) {
  if (out_diagnostic) out_diagnostic->Clear();

  if (!out_plan) {
    if (out_error) *out_error = "Null out_plan pointer";
    if (out_diagnostic) {
      out_diagnostic->code = "DEPLOYMENT_ERROR";
      out_diagnostic->path = "/";
      out_diagnostic->message = "Null out_plan pointer";
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
    }
    return -2;
  }

  return ResolveFromPipelineJson(raw_pipe_json, model_root_dir, out_plan,
                                 out_error, out_diagnostic, output_pool_depth);
}

int IoPlanResolver::ResolveFromPipelineJson(
    const nlohmann::json& pipeline_json, const std::string& model_root_dir,
    std::unique_ptr<ValidatedIoPlan>* out_plan, std::string* out_error,
    DeploymentDiagnostic* out_diagnostic, uint32_t output_pool_depth) {
  if (out_diagnostic) out_diagnostic->Clear();

  if (!out_plan) {
    if (out_error) *out_error = "Null out_plan pointer";
    if (out_diagnostic) {
      out_diagnostic->code = "DEPLOYMENT_ERROR";
      out_diagnostic->path = "/";
      out_diagnostic->message = "Null out_plan pointer";
    }
    return -1;
  }
  *out_plan = nullptr;

  DeploymentPrepareOptions options;

  options.model_root_dir = model_root_dir;

  PreparedDeployment prepared;
  DeploymentDiagnostic prep_diag;
  if (!PrepareDeploymentDocument(pipeline_json, options, &prepared,
                                 &prep_diag)) {
    if (out_error)
      *out_error =
          prep_diag.code + " at " + prep_diag.path + ": " + prep_diag.message;
    if (out_diagnostic) *out_diagnostic = prep_diag;
    return prep_diag.pipeline_diagnostic ? -3 : -2;
  }

  output_pool_depth =
      output_pool_depth ? output_pool_depth : kDefaultOutputPoolDepth;
  if (output_pool_depth > kMaxOutputPoolDepth) {
    if (out_error) *out_error = "output_pool_depth exceeds hard limit";
    if (out_diagnostic) {
      out_diagnostic->code = "DEPLOYMENT_ERROR";
      out_diagnostic->path = "/";
      out_diagnostic->message = "output_pool_depth exceeds hard limit";
    }
    return -2;
  }

  // 按本次有效深度检查句柄池总预算
  size_t total_handle_pool_bytes = 0;
  for (size_t index = 0; index < prepared.outputs.size(); ++index) {
    const auto& pool_spec = prepared.outputs[index].pool_spec;
    const auto* output_binding =
        OperatorValueTypeRegistry::Instance().GetOutputBinding(
            pool_spec.type, pool_spec.allocator);
    if (!output_binding || output_binding->direction != IoDirection::kOutput) {
      std::string msg =
          "Missing output value binding for suffix '" + pool_spec.type + "'";
      if (out_error) *out_error = msg;
      if (out_diagnostic) {
        out_diagnostic->code = "INVALID_OUTPUT_ALLOCATION";
        out_diagnostic->path = "/io/output/" + std::to_string(index);
        out_diagnostic->message = msg;
      }
      return -2;
    }
    size_t slot_pool_bytes = 0;
    std::string budget_err;
    if (!ComputeOutputPoolPayloadBytes(*output_binding, pool_spec,
                                       output_pool_depth, &slot_pool_bytes,
                                       &budget_err)) {
      std::string msg = "Output pool budget calculation failed: " + budget_err;
      if (out_error) *out_error = msg;
      if (out_diagnostic) {
        out_diagnostic->code = "INVALID_OUTPUT_ALLOCATION";
        out_diagnostic->path = "/io/output/" + std::to_string(index);
        out_diagnostic->message = msg;
      }
      return -2;
    }
    if (!CheckedAdd(total_handle_pool_bytes, slot_pool_bytes,
                    &total_handle_pool_bytes)) {
      std::string msg = "Handle pool budget addition overflowed";
      if (out_error) *out_error = msg;
      if (out_diagnostic) {
        out_diagnostic->code = "INVALID_OUTPUT_ALLOCATION";
        out_diagnostic->path = "/io/output";
        out_diagnostic->message = msg;
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
      out_diagnostic->path = "/io/output";
      out_diagnostic->message = msg;
    }
    return -2;
  }

  // 调用 PipelineValidator 进行统一中性计划验证 (Core 校验
  // neutral_pipeline_json)
  auto plan = std::make_unique<ValidatedPipelinePlan>(
      PipelineValidator::ValidateAndPlan(prepared.neutral_pipeline_json,
                                         prepared.io_boundary));

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
      }
    }
    return -3;
  }

  // 组装不可变接入计划
  auto io_plan = std::make_unique<ValidatedIoPlan>();
  static_cast<IoSelection&>(*io_plan) =
      std::move(static_cast<IoSelection&>(prepared));
  io_plan->resolved_pipeline_json = std::move(prepared.neutral_pipeline_json);
  auto io = nlohmann::json::object();
  io["input"] = nlohmann::json::array();
  io["output"] = nlohmann::json::array();
  for (const auto& selected : io_plan->inputs) {
    const auto& def = *selected.converter;
    io["input"].push_back({{"type", def.type},
                           {"name", def.name},
                           {"params", selected.params->Effective()}});
  }
  for (const auto& selected : io_plan->outputs) {
    const auto& def = *selected.converter;
    io["output"].push_back({{"type", def.type},
                            {"name", def.name},
                            {"params", selected.params->Effective()}});
  }
  io_plan->resolved_pipeline_json["io"] = std::move(io);
  io_plan->pipeline_plan = std::move(plan);

  *out_plan = std::move(io_plan);
  return 0;
}

}  // namespace llm_edgeflow

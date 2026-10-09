#include "adapter/shared_algorithm_runtime.h"

#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/io_converter_registry.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "contracts/diagnostic.h"
#include "core/alg_context.h"
#include "core/diagnostic_code.h"
#include "core/node_registry.h"
#include "core/pipeline_validator.h"
#include "core/session_context.h"
#include "edgeflow/log.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {

namespace {

// 按注册表标明来源，收集已记录的冲突原因。
template <typename Registry>
void CollectConflicts(const char* registry_name, const Registry& registry,
                      std::vector<std::string>* errors) {
  if (!registry.HasConflict()) return;
  for (const auto& message : registry.GetConflictErrors()) {
    errors->push_back(std::string(registry_name) + ": " + message);
  }
}

}  // namespace

int SharedAlgorithmRuntime::GlobalInit(std::string* diagnostic) noexcept {
  try {
    if (diagnostic) diagnostic->clear();
    // 一次收集全部问题：任一注册表冲突或绑定审计失败都使全局初始化失败。
    std::vector<std::string> errors;
    CollectConflicts("NodeRegistry", NodeRegistry::Instance(), &errors);
    CollectConflicts("ModelRegistry", ModelRegistry::Instance(), &errors);
    CollectConflicts("BackendRegistry", BackendRegistry::Instance(), &errors);
    std::vector<std::string> model_audit_errors;
    if (!ModelRegistry::Instance().Audit(BackendRegistry::Instance(),
                                         &model_audit_errors))
      for (auto& error : model_audit_errors)
        errors.push_back("Model audit: " + std::move(error));
    if (OperatorValueTypeRegistry::Instance().HasConflict()) {
      errors.push_back("OperatorValueTypeRegistry: registration conflict");
    }
    // 审计全部 Converter 的平台结构、参数与端口契约。
    std::vector<std::string> audit_errors;
    if (!IoConverterRegistry::Instance().Audit(&audit_errors)) {
      for (auto& error : audit_errors) {
        errors.push_back("Converter audit: " + std::move(error));
      }
    }
    if (errors.empty()) return 0;

    ALG_LOG_ERROR("[SharedAlgorithmRuntime] GlobalInit failed:\n");
    std::string message;
    for (const auto& error : errors) {
      ALG_LOG_ERROR("  - %s\n", error.c_str());
      if (!message.empty()) message += "; ";
      message += error;
    }
    if (diagnostic) *diagnostic = std::move(message);
    return COMPANY_ALG_ERR_REGISTRY_CONFLICT;  // -6
  } catch (const std::exception& e) {
    ALG_LOG_ERROR("[SharedAlgorithmRuntime] GlobalInit exception: %s\n",
                  e.what());
    SetDiagnosticNoexcept(diagnostic, e.what());
    return COMPANY_ALG_ERR_EXCEPTION;
  } catch (...) {
    SetDiagnosticNoexcept(diagnostic, "Unknown exception in GlobalInit");
    return COMPANY_ALG_ERR_UNKNOWN;
  }
}

int SharedAlgorithmRuntime::CreateFromIoPlan(
    std::unique_ptr<ValidatedIoPlan> io_plan, int device_id,
    const RuntimeOptions* extra_runtime_options,
    std::unique_ptr<SharedAlgorithmRuntime>* out_runtime,
    std::string* out_error) noexcept {
  try {
    if (!out_runtime) {
      if (out_error) *out_error = "Null out_runtime pointer";
      return COMPANY_ALG_ERR_INVALID_HANDLE;  // -1
    }
    *out_runtime = nullptr;

    if (!io_plan || io_plan->inputs.empty() || io_plan->outputs.empty() ||
        !io_plan->pipeline_plan) {
      if (out_error) *out_error = "Invalid or incomplete ValidatedIoPlan";
      return COMPANY_ALG_ERR_INVALID_PARAM;  // -2
    }

    auto pipeline = std::make_unique<Pipeline>();

    RuntimeOptions options;
    if (extra_runtime_options) {
      options = *extra_runtime_options;
    }
    options.device_id = device_id;
    options.has_device_id = (device_id >= 0);

    pipeline->GetSessionContext().SetRuntimeOptions(options);

    PipelineDiagnostic diagnostic;
    if (!pipeline->BuildFromPlan(std::move(io_plan->pipeline_plan),
                                 &diagnostic)) {
      if (out_error) {
        *out_error =
            "Failed to build pipeline from plan: " + diagnostic.message +
            " (code: " + std::string(DiagnosticCodeName(diagnostic.code)) +
            ", path: " + diagnostic.path + ")";
      }
      if (diagnostic.code == DiagnosticCode::kRegistryConflict) {
        return COMPANY_ALG_ERR_REGISTRY_CONFLICT;  // -6
      }
      return COMPANY_ALG_ERR_INVALID_INPUT;  // -3
    }

    auto runtime = std::make_unique<SharedAlgorithmRuntime>();
    runtime->io_plan_ = std::move(io_plan);
    runtime->pipeline_ = std::move(pipeline);

    *out_runtime = std::move(runtime);
    return COMPANY_ALG_SUCCESS;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(out_error, e.what());
    return COMPANY_ALG_ERR_EXCEPTION;
  } catch (...) {
    SetDiagnosticNoexcept(out_error, "Unknown exception");
    return COMPANY_ALG_ERR_UNKNOWN;
  }
}

int SharedAlgorithmRuntime::ExecuteControl(
    int cmd, const std::string& json_param_str, std::string* out_error,
    ControlFailureStage* failure_stage) noexcept {
  if (failure_stage) *failure_stage = ControlFailureStage::kNone;
  try {
    if (!pipeline_) {
      if (out_error) *out_error = "Null pipeline in runtime instance";
      return COMPANY_ALG_ERR_INVALID_HANDLE;  // -1
    }
    return pipeline_->Control(cmd, json_param_str, out_error, failure_stage);
  } catch (const std::exception& e) {
    if (failure_stage) *failure_stage = ControlFailureStage::kNone;
    SetDiagnosticNoexcept(out_error, e.what());
    return COMPANY_ALG_ERR_EXCEPTION;
  } catch (...) {
    if (failure_stage) *failure_stage = ControlFailureStage::kNone;
    SetDiagnosticNoexcept(out_error, "Unknown exception");
    return COMPANY_ALG_ERR_UNKNOWN;
  }
}

}  // namespace llm_edgeflow

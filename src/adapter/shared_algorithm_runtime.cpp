#include "adapter/shared_algorithm_runtime.h"

#include <cstring>
#include <fstream>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/deployment_model_resolver.h"
#include "adapter/io_binding_registry.h"
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

int SharedAlgorithmRuntime::GlobalInit() noexcept {
  try {
    // 1. NodeRegistry 冲突审计
    if (NodeRegistry::Instance().HasConflict()) {
      ALG_LOG_ERROR(
          "[SharedAlgorithmRuntime] GlobalInit failed: Registration conflict "
          "in NodeRegistry.\n");
      return COMPANY_ALG_ERR_REGISTRY_CONFLICT;  // -6
    }

    // 2. Model/Backend Registry 冲突审计
    if (ModelRegistry::Instance().HasConflict() ||
        BackendRegistry::Instance().HasConflict()) {
      ALG_LOG_ERROR(
          "[SharedAlgorithmRuntime] GlobalInit failed: Registration conflict "
          "in ModelRegistry or BackendRegistry.\n");
      return COMPANY_ALG_ERR_REGISTRY_CONFLICT;  // -6
    }

    // 3. OperatorValueTypeRegistry 冲突审计
    if (OperatorValueTypeRegistry::Instance().HasConflict()) {
      ALG_LOG_ERROR(
          "[SharedAlgorithmRuntime] GlobalInit failed: Registration conflict "
          "in OperatorValueTypeRegistry.\n");
      return COMPANY_ALG_ERR_REGISTRY_CONFLICT;  // -6
    }

    // 4. IoConverterRegistry 冲突审计
    if (IoConverterRegistry::Instance().HasConflict()) {
      ALG_LOG_ERROR(
          "[SharedAlgorithmRuntime] GlobalInit failed: Registration conflict "
          "in IoConverterRegistry.\n");
      return COMPANY_ALG_ERR_REGISTRY_CONFLICT;  // -6
    }

    // 5. IoBindingRegistry 冲突审计
    if (IoBindingRegistry::Instance().HasConflict()) {
      ALG_LOG_ERROR(
          "[SharedAlgorithmRuntime] GlobalInit failed: Registration conflict "
          "in IoBindingRegistry.\n");
      return COMPANY_ALG_ERR_REGISTRY_CONFLICT;  // -6
    }

    // 6. IoBinding 全量接入审计
    std::vector<std::string> audit_errors;
    if (!IoBindingRegistry::Instance().Audit(&audit_errors)) {
      ALG_LOG_ERROR(
          "[SharedAlgorithmRuntime] GlobalInit failed: IoBindingRegistry Audit "
          "failed:\n");
      for (const auto& err : audit_errors) {
        ALG_LOG_ERROR("  - %s\n", err.c_str());
      }
      return COMPANY_ALG_ERR_REGISTRY_CONFLICT;  // -6
    }

    return 0;
  } catch (const std::exception& e) {
    ALG_LOG_ERROR("[SharedAlgorithmRuntime] GlobalInit exception: %s\n",
                  e.what());
    return COMPANY_ALG_ERR_EXCEPTION;
  } catch (...) {
    return COMPANY_ALG_ERR_UNKNOWN;
  }
}

int SharedAlgorithmRuntime::GlobalDeinit() noexcept {
  try {
    return 0;
  } catch (...) {
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

    if (!io_plan || !io_plan->input_converter || !io_plan->output_converter ||
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
    options.biz_name = io_plan->binding.biz_name;

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

int SharedAlgorithmRuntime::ExecuteControl(int cmd,
                                           const std::string& json_param_str,
                                           std::string* out_error) noexcept {
  try {
    if (!pipeline_) {
      if (out_error) *out_error = "Null pipeline in runtime instance";
      return COMPANY_ALG_ERR_INVALID_HANDLE;  // -1
    }
    return pipeline_->Control(cmd, json_param_str, out_error);
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(out_error, e.what());
    return COMPANY_ALG_ERR_EXCEPTION;
  } catch (...) {
    SetDiagnosticNoexcept(out_error, "Unknown exception");
    return COMPANY_ALG_ERR_UNKNOWN;
  }
}

}  // namespace llm_edgeflow

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

int SharedAlgorithmRuntime::CreateFromConfigFile(
    const std::string& config_path, int device_id,
    const std::string& model_root_dir,
    std::unique_ptr<SharedAlgorithmRuntime>* out_runtime,
    std::string* out_error) noexcept {
  try {
    if (!out_runtime) {
      if (out_error) *out_error = "Null out_runtime pointer";
      return COMPANY_ALG_ERR_INVALID_HANDLE;  // -1
    }
    *out_runtime = nullptr;

    if (config_path.empty()) {
      if (out_error) *out_error = "Empty config_file_path";
      return COMPANY_ALG_ERR_INVALID_PARAM;  // -2
    }

    std::unique_ptr<ValidatedIoPlan> io_plan;
    std::string resolve_err;
    int ret = IoBindingResolver::ResolveFromFile(
        config_path, "cabi", model_root_dir, &io_plan, &resolve_err);
    if (ret != 0) {
      if (out_error) *out_error = resolve_err;
      return ret;
    }

    return CreateFromIoPlan(std::move(io_plan), device_id, nullptr, out_runtime,
                            out_error);
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(out_error, e.what());
    return COMPANY_ALG_ERR_EXCEPTION;
  } catch (...) {
    SetDiagnosticNoexcept(out_error, "Unknown exception");
    return COMPANY_ALG_ERR_UNKNOWN;
  }
}

int SharedAlgorithmRuntime::CreateFromPipelineJson(
    const nlohmann::json& pipeline_json, int device_id,
    const std::string& model_root_dir, const std::string& binding_id,
    std::unique_ptr<SharedAlgorithmRuntime>* out_runtime,
    std::string* out_error,
    const RuntimeOptions* extra_runtime_options) noexcept {
  try {
    if (!out_runtime) {
      if (out_error) *out_error = "Null out_runtime pointer";
      return COMPANY_ALG_ERR_INVALID_HANDLE;  // -1
    }
    *out_runtime = nullptr;

    if (binding_id.empty()) {
      if (out_error) *out_error = "binding_id must not be empty";
      return COMPANY_ALG_ERR_INVALID_PARAM;  // -2
    }

    std::unique_ptr<ValidatedIoPlan> io_plan;
    std::string resolve_err;
    int ret = IoBindingResolver::ResolveFromPipelineJson(
        pipeline_json, binding_id, "cabi", model_root_dir, &io_plan,
        &resolve_err);
    if (ret != 0) {
      if (out_error) *out_error = resolve_err;
      return COMPANY_ALG_ERR_INVALID_PARAM;  // -2
    }

    return CreateFromIoPlan(std::move(io_plan), device_id,
                            extra_runtime_options, out_runtime, out_error);
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(out_error, e.what());
    return COMPANY_ALG_ERR_EXCEPTION;
  } catch (...) {
    SetDiagnosticNoexcept(out_error, "Unknown exception");
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

int SharedAlgorithmRuntime::ExecuteBatch(const void** inputs, int num_inputs,
                                         void** outputs, int* num_outputs,
                                         std::string* out_error) noexcept {
  try {
    if (!io_plan_ || !pipeline_) {
      if (out_error)
        *out_error = "Null IO plan or pipeline in runtime instance";
      return COMPANY_ALG_ERR_INVALID_HANDLE;  // -1
    }

    // 1. 批大小与槽位容量预检
    int preflight_ret = AdapterValidationHelper::ValidateBatchPreFlight(
        inputs, num_inputs, outputs, num_outputs,
        static_cast<int>(io_plan_->effective_max_batch_size), num_inputs,
        io_plan_->binding.binding_id.c_str());
    if (preflight_ret != 0) {
      if (out_error) {
        *out_error = "ValidateBatch preflight failed with code " +
                     std::to_string(preflight_ret);
      }
      if (num_outputs && *num_outputs >= 0 &&
          preflight_ret != COMPANY_ALG_ERR_BUFFER_TOO_SMALL) {
        *num_outputs = 0;
      }
      return preflight_ret;
    }

    // 2. 解包到 AlgContext 请求黑板
    AlgContext req_ctx;
    ExternalInputBatchView in_view;
    in_view.items = inputs;
    in_view.count = static_cast<size_t>(num_inputs);
    in_view.type_id = io_plan_->input_converter->external_type;

    InputDecodeOptions in_options;
    in_options.binding_id = io_plan_->binding.binding_id;
    in_options.converter_id = io_plan_->input_converter->converter_id;
    in_options.transport = "cabi";
    in_options.max_batch_size = io_plan_->effective_max_batch_size;

    AdapterStatus decode_status;
    int decode_ret = io_plan_->input_converter->decode_fn(
        in_view, in_options, io_plan_->input_port_bindings, &req_ctx,
        &decode_status);
    if (decode_ret != 0) {
      if (num_outputs) *num_outputs = 0;
      if (out_error) {
        *out_error = "DecodeInput failed for " +
                     io_plan_->input_converter->converter_id + ": " +
                     decode_status.ToString();
      }
      return decode_ret;
    }

    // 3. 执行 Pipeline DAG 计算
    int exec_ret = pipeline_->Execute(&req_ctx);
    if (exec_ret != 0) {
      if (num_outputs) *num_outputs = 0;
      if (out_error) {
        *out_error = "Pipeline::Execute failed with code " +
                     std::to_string(exec_ret) + ": " +
                     req_ctx.GetErrorMessage();
      }
      return exec_ret;
    }

    // 4. 打包回 C 结构体输出
    ExternalOutputBatchView out_view;
    out_view.items = outputs;
    out_view.count = static_cast<size_t>(num_inputs);
    out_view.capacity = static_cast<size_t>(*num_outputs);
    out_view.type_id = io_plan_->output_converter->external_type;

    OutputEncodeOptions out_options;
    out_options.binding_id = io_plan_->binding.binding_id;
    out_options.converter_id = io_plan_->output_converter->converter_id;
    out_options.transport = "cabi";
    out_options.max_batch_size = io_plan_->effective_max_batch_size;

    size_t written_count = 0;
    AdapterStatus encode_status;
    int encode_ret = io_plan_->output_converter->encode_fn(
        &req_ctx, io_plan_->output_port_bindings, out_options, &out_view,
        &written_count, &encode_status);
    if (encode_ret != 0) {
      if (encode_ret == COMPANY_ALG_ERR_BUFFER_TOO_SMALL ||
          encode_status.Code() == COMPANY_ALG_ERR_BUFFER_TOO_SMALL) {
        if (num_outputs) *num_outputs = num_inputs;
      } else {
        if (num_outputs) *num_outputs = 0;
      }
      if (out_error) {
        *out_error = "EncodeOutput failed for " +
                     io_plan_->output_converter->converter_id + ": " +
                     encode_status.ToString();
      }
      return encode_ret;
    }

    if (written_count != static_cast<size_t>(num_inputs)) {
      if (num_outputs) *num_outputs = 0;
      if (out_error) {
        *out_error =
            "EncodeOutput written count (" + std::to_string(written_count) +
            ") does not match input count (" + std::to_string(num_inputs) + ")";
      }
      return COMPANY_ALG_ERR_UNKNOWN;
    }

    if (num_outputs) *num_outputs = static_cast<int>(written_count);
    return COMPANY_ALG_SUCCESS;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(out_error, e.what());
    if (num_outputs) *num_outputs = 0;
    return COMPANY_ALG_ERR_EXCEPTION;
  } catch (...) {
    SetDiagnosticNoexcept(out_error, "Unknown exception");
    if (num_outputs) *num_outputs = 0;
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

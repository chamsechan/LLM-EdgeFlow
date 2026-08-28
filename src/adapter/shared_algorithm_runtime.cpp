#include "adapter/shared_algorithm_runtime.h"

#include <cstring>

#include "adapter/biz_adapter_registry.h"
#include "adapter/operator/operator_biz_bridge_registry.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "company_alg_log.h"
#include "core/alg_context.h"
#include "core/node_registry.h"
#include "core/session_context.h"
#include "engine/engine_registry.h"

namespace alg_framework {

int SharedAlgorithmRuntime::GlobalInit() noexcept {
  try {
    // 1. BizAdapterRegistry 冲突审计
    if (BizAdapterRegistry::Instance().HasRegistrationConflict()) {
      ALG_LOG_ERROR(
          "[SharedAlgorithmRuntime] GlobalInit failed: Registration conflict "
          "in BizAdapterRegistry.\n");
      return COMPANY_ALG_ERR_REGISTRY_CONFLICT;  // -6
    }

    // 2. NodeFactory 冲突审计
    if (NodeFactory::Instance().HasConflict()) {
      ALG_LOG_ERROR(
          "[SharedAlgorithmRuntime] GlobalInit failed: Registration conflict "
          "in NodeFactory.\n");
      return COMPANY_ALG_ERR_REGISTRY_CONFLICT;  // -6
    }

    // 3. EngineFactory 冲突审计
    if (EngineFactory::Instance().HasConflict()) {
      ALG_LOG_ERROR(
          "[SharedAlgorithmRuntime] GlobalInit failed: Registration conflict "
          "in EngineFactory.\n");
      return COMPANY_ALG_ERR_REGISTRY_CONFLICT;  // -6
    }

    // 4. OperatorValueTypeRegistry 冲突审计
    if (OperatorValueTypeRegistry::Instance().HasConflict()) {
      ALG_LOG_ERROR(
          "[SharedAlgorithmRuntime] GlobalInit failed: Registration conflict "
          "in OperatorValueTypeRegistry.\n");
      return COMPANY_ALG_ERR_REGISTRY_CONFLICT;  // -6
    }

    // 5. OperatorBizBridgeRegistry 冲突审计
    if (OperatorBizBridgeRegistry::Instance().HasConflict()) {
      ALG_LOG_ERROR(
          "[SharedAlgorithmRuntime] GlobalInit failed: Registration conflict "
          "in OperatorBizBridgeRegistry.\n");
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
    const std::string& model_root_dir, CompanyAlgBizType biz_type,
    std::unique_ptr<SharedAlgorithmRuntime>* out_runtime,
    std::string* out_error) noexcept {
  try {
    if (!out_runtime) {
      if (out_error) *out_error = "Null out_runtime pointer";
      return COMPANY_ALG_ERR_INVALID_HANDLE;  // -1
    }
    *out_runtime = nullptr;

    if (biz_type == ALG_BIZ_TYPE_UNKNOWN) {
      if (out_error) *out_error = "Cannot create with ALG_BIZ_TYPE_UNKNOWN";
      return COMPANY_ALG_ERR_UNSUPPORTED_BIZ;  // -5
    }

    auto adapter = BizAdapterRegistry::Instance().GetAdapter(biz_type);
    if (!adapter) {
      if (out_error) {
        *out_error =
            "Unsupported or unregistered biz_type: " + std::to_string(biz_type);
      }
      return COMPANY_ALG_ERR_UNSUPPORTED_BIZ;  // -5
    }

    if (config_path.empty()) {
      if (out_error) *out_error = "Empty config_file_path";
      return COMPANY_ALG_ERR_INVALID_PARAM;  // -2
    }

    auto runtime = std::make_unique<SharedAlgorithmRuntime>();
    runtime->biz_type_ = biz_type;
    runtime->device_id_ = device_id;
    runtime->model_root_dir_ = model_root_dir;
    runtime->config_file_path_ = config_path;
    runtime->adapter_ = adapter;
    runtime->pipeline_ = std::make_unique<Pipeline>();

    RuntimeOptions options;
    options.config_file_path = config_path;
    options.model_root_dir = model_root_dir;
    options.device_id = device_id;
    options.has_device_id = (device_id >= 0);
    options.biz_type = static_cast<int>(biz_type);
    options.business_name = adapter->BizName();

    runtime->pipeline_->GetSessionContext().SetRuntimeOptions(options);

    PipelineDiagnostic diagnostic;
    if (!runtime->pipeline_->BuildFromConfigFile(config_path, &diagnostic)) {
      if (out_error) {
        *out_error =
            "Failed to build pipeline from config: " + diagnostic.message +
            " (code: " + std::to_string(static_cast<int>(diagnostic.code)) +
            ", path: " + diagnostic.path + ")";
      }
      if (diagnostic.code == PipelineErrorCode::kRegistryConflict) {
        return COMPANY_ALG_ERR_REGISTRY_CONFLICT;  // -6
      }
      // 保持 main 既有纯 C ABI 契约：只要 BuildFromConfigFile 失败，
      // 文件打开、JSON 解析和配置语义错误均返回 -3。
      return COMPANY_ALG_ERR_INVALID_INPUT;  // -3
    }

    if (!adapter->ValidatePipelineBinding(
            runtime->pipeline_->GetBusinessName())) {
      if (out_error) {
        *out_error = "Pipeline business_name '" +
                     runtime->pipeline_->GetBusinessName() +
                     "' does not match adapter '" + adapter->BizName() + "'";
      }
      return COMPANY_ALG_ERR_UNSUPPORTED_BIZ;  // -5
    }

    *out_runtime = std::move(runtime);
    return COMPANY_ALG_SUCCESS;
  } catch (const std::exception& e) {
    if (out_error) *out_error = std::string("Exception: ") + e.what();
    return COMPANY_ALG_ERR_EXCEPTION;
  } catch (...) {
    if (out_error) *out_error = "Unknown exception in CreateFromConfigFile";
    return COMPANY_ALG_ERR_UNKNOWN;
  }
}

int SharedAlgorithmRuntime::CreateFromPipelineJson(
    const nlohmann::json& pipeline_json, int device_id,
    const std::string& model_root_dir, CompanyAlgBizType biz_type,
    std::unique_ptr<SharedAlgorithmRuntime>* out_runtime,
    std::string* out_error,
    const RuntimeOptions* extra_runtime_options) noexcept {
  try {
    if (!out_runtime) {
      if (out_error) *out_error = "Null out_runtime pointer";
      return COMPANY_ALG_ERR_INVALID_HANDLE;  // -1
    }
    *out_runtime = nullptr;

    if (biz_type == ALG_BIZ_TYPE_UNKNOWN) {
      if (out_error) *out_error = "Cannot create with ALG_BIZ_TYPE_UNKNOWN";
      return COMPANY_ALG_ERR_UNSUPPORTED_BIZ;  // -5
    }

    auto adapter = BizAdapterRegistry::Instance().GetAdapter(biz_type);
    if (!adapter) {
      if (out_error) {
        *out_error =
            "Unsupported or unregistered biz_type: " + std::to_string(biz_type);
      }
      return COMPANY_ALG_ERR_UNSUPPORTED_BIZ;  // -5
    }

    auto runtime = std::make_unique<SharedAlgorithmRuntime>();
    runtime->biz_type_ = biz_type;
    runtime->device_id_ = device_id;
    runtime->model_root_dir_ = model_root_dir;
    runtime->adapter_ = adapter;
    runtime->pipeline_ = std::make_unique<Pipeline>();

    RuntimeOptions options;
    if (extra_runtime_options) {
      options = *extra_runtime_options;
    }
    options.config_file_path = "<in_memory_json>";
    options.model_root_dir = model_root_dir;
    options.device_id = device_id;
    options.has_device_id = (device_id >= 0);
    options.biz_type = static_cast<int>(biz_type);
    options.business_name = adapter->BizName();

    runtime->pipeline_->GetSessionContext().SetRuntimeOptions(options);

    PipelineDiagnostic diagnostic;
    if (!runtime->pipeline_->BuildFromJson(pipeline_json, &diagnostic)) {
      if (out_error) {
        *out_error =
            "Failed to build pipeline from JSON: " + diagnostic.message +
            " (code: " + std::to_string(static_cast<int>(diagnostic.code)) +
            ", path: " + diagnostic.path + ")";
      }
      if (diagnostic.code == PipelineErrorCode::kRegistryConflict) {
        return COMPANY_ALG_ERR_REGISTRY_CONFLICT;  // -6
      }
      return COMPANY_ALG_ERR_INVALID_PARAM;  // -2
    }

    if (!adapter->ValidatePipelineBinding(
            runtime->pipeline_->GetBusinessName())) {
      if (out_error) {
        *out_error = "Pipeline business_name '" +
                     runtime->pipeline_->GetBusinessName() +
                     "' does not match adapter '" + adapter->BizName() + "'";
      }
      return COMPANY_ALG_ERR_UNSUPPORTED_BIZ;  // -5
    }

    *out_runtime = std::move(runtime);
    return COMPANY_ALG_SUCCESS;
  } catch (const std::exception& e) {
    if (out_error) *out_error = std::string("Exception: ") + e.what();
    return COMPANY_ALG_ERR_EXCEPTION;
  } catch (...) {
    if (out_error) *out_error = "Unknown exception in CreateFromPipelineJson";
    return COMPANY_ALG_ERR_UNKNOWN;
  }
}

int SharedAlgorithmRuntime::ExecuteBatch(const void** inputs, int num_inputs,
                                         void** outputs, int* num_outputs,
                                         std::string* out_error) noexcept {
  try {
    if (!adapter_) {
      if (out_error) *out_error = "Null business adapter in runtime instance";
      return COMPANY_ALG_ERR_UNSUPPORTED_BIZ;  // -5
    }

    // 1. 批大小与槽位容量预检
    int preflight_ret =
        adapter_->ValidateBatch(inputs, num_inputs, outputs, num_outputs);
    if (preflight_ret != 0) {
      if (out_error) {
        *out_error = "ValidateBatch preflight failed with code " +
                     std::to_string(preflight_ret);
      }
      return preflight_ret;
    }

    // 2. 解包到 AlgContext 请求黑板
    AlgContext req_ctx;
    AdapterStatus unpack_status;
    int unpack_ret =
        adapter_->Unpack(inputs, num_inputs, &req_ctx, &unpack_status);
    if (unpack_ret != 0) {
      if (out_error) {
        *out_error = "Unpack failed for " + std::string(adapter_->BizName()) +
                     ": " + unpack_status.ToString();
      }
      return unpack_ret;
    }

    // 3. 执行 Pipeline DAG 计算
    int exec_ret = pipeline_->Execute(&req_ctx);
    if (exec_ret != 0) {
      if (out_error) {
        *out_error = "Pipeline::Execute failed with code " +
                     std::to_string(exec_ret) + ": " +
                     req_ctx.GetErrorMessage();
      }
      return exec_ret;
    }

    // 4. 打包回 C 结构体输出
    AdapterStatus pack_status;
    int pack_ret = adapter_->Pack(&req_ctx, outputs, num_outputs, &pack_status);
    if (pack_ret != 0) {
      if (out_error) {
        *out_error = "Pack failed for " + std::string(adapter_->BizName()) +
                     ": " + pack_status.ToString();
      }
      return pack_ret;
    }

    return COMPANY_ALG_SUCCESS;
  } catch (const std::exception& e) {
    if (out_error) *out_error = std::string("Exception: ") + e.what();
    return COMPANY_ALG_ERR_EXCEPTION;
  } catch (...) {
    if (out_error) *out_error = "Unknown exception in ExecuteBatch";
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
    return pipeline_->Control(cmd, json_param_str);
  } catch (const std::exception& e) {
    if (out_error) *out_error = std::string("Exception: ") + e.what();
    return COMPANY_ALG_ERR_EXCEPTION;
  } catch (...) {
    if (out_error) *out_error = "Unknown exception in ExecuteControl";
    return COMPANY_ALG_ERR_UNKNOWN;
  }
}

}  // namespace alg_framework

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "adapter/io_plan_resolver.h"
#include "adapter/operator/mock/operator_control_registry.h"
#include "adapter/operator/mock/operator_error_mapping.h"
#include "adapter/operator/mock/operator_process_binding.h"
#include "adapter/shared_algorithm_runtime.h"
#include "contracts/diagnostic.h"
#include "contracts/error_codes.h"
#include "edgeflow/log.h"
#include "edgeflow/operator/interface.h"
#include "platform_mock/error_codes.h"

namespace llm_edgeflow::operator_api {

namespace {

thread_local std::string g_last_operator_error;

int Fail(const char* operation, int code, std::string_view detail,
         OperatorFailureStage stage = OperatorFailureStage::kRuntime) noexcept {
  const int public_code = PublicFailureCode(stage, code);
  try {
    g_last_operator_error = DescribeOperatorFailure(stage, code, detail);
  } catch (...) {
    SetDiagnosticNoexcept(&g_last_operator_error, detail);
  }
  const bool rejected =
      (public_code == COMPANY_ALG_ERR_INVALID_HANDLE ||
       public_code == COMPANY_ALG_ERR_INVALID_PARAM ||
       public_code == COMPANY_ALG_ERR_INVALID_INPUT ||
       public_code == COMPANY_ALG_ERR_BUFFER_TOO_SMALL ||
       public_code == COMPANY_ALG_ERR_UNSUPPORTED_BIZ ||
       public_code == COMPANY_ALG_ERR_UNSUPPORTED_CONTROL) &&
      code != error::kAllocationFailed && code != error::kPoolCorrupt &&
      code != error::kPoolMissing &&
      code != error::kModelMaterializationFailed &&
      code != error::kNodeCreateFailed && code != error::kNodeInitFailed;
  // 边界日志只记录操作、码名/码值、平台码和失败阶段；detail 可能携带请求内容
  // （如 JSON 解析错误中的原文片段），只写入调用者可读的
  // GetOperatorLastError()。
  if (rejected) {
    ALG_LOG_WARNING(
        "[Operator] operation=%s stage=%s code=%s(%d) platform_code=%d\n",
        operation, OperatorFailureStageName(stage), ErrorCodeName(code), code,
        public_code);
  } else {
    ALG_LOG_ERROR(
        "[Operator] operation=%s stage=%s code=%s(%d) platform_code=%d\n",
        operation, OperatorFailureStageName(stage), ErrorCodeName(code), code,
        public_code);
  }
  return public_code;
}

struct OperatorHandle {
  std::unique_ptr<llm_edgeflow::SharedAlgorithmRuntime> runtime;
};

/**
 * @brief 全局线程安全活跃句柄注册中心 (杜绝 Use-After-Free 与悬挂指针解引用)
 */
class OperatorHandleManager {
 public:
  static OperatorHandleManager& Instance() {
    static OperatorHandleManager instance;
    return instance;
  }

  bool Register(OperatorHandle* h) {
    if (!h) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return active_handles_.insert(h).second;
  }

  bool IsValid(void* handle) {
    if (!handle) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return active_handles_.find(static_cast<OperatorHandle*>(handle)) !=
           active_handles_.end();
  }

  OperatorHandle* ExtractForDestroy(void* handle) {
    if (!handle) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_handles_.find(static_cast<OperatorHandle*>(handle));
    if (it == active_handles_.end()) {
      return nullptr;
    }
    OperatorHandle* h = *it;
    active_handles_.erase(it);
    return h;
  }

  int DestroyAll(std::string* diagnostic) noexcept {
    try {
      std::unordered_set<OperatorHandle*> to_destroy;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        to_destroy.swap(active_handles_);
      }

      int first_error = 0;
      for (auto* h : to_destroy) {
        if (!h) continue;
        std::unique_ptr<OperatorHandle> owner(h);
        std::string close_error;
        const int result = owner->runtime->Close(&close_error);
        if (result != 0 && first_error == 0) {
          first_error = result;
          SetDiagnosticNoexcept(diagnostic, close_error);
        }
      }
      return first_error;
    } catch (...) {
      SetDiagnosticNoexcept(diagnostic,
                            "Unknown exception closing remaining runtimes");
      return error::kUnknownException;
    }
  }

 private:
  std::mutex mutex_;
  std::unordered_set<OperatorHandle*> active_handles_;
};

int Operator_Init() noexcept {
  try {
    std::string diagnostic;
    const int result =
        llm_edgeflow::SharedAlgorithmRuntime::GlobalInit(&diagnostic);
    if (result != 0) return Fail(__func__, result, diagnostic);
    ALG_LOG_INFO("[Operator] GlobalInit completed\n");
    return COMPANY_ALG_SUCCESS;
  } catch (const std::exception& e) {
    return Fail(__func__, error::kInternalException, e.what());
  } catch (...) {
    return Fail(__func__, error::kUnknownException,
                "Unknown exception in Init");
  }
}

int Operator_Create(void** handle, const void* create_param) noexcept {
  try {
    const auto* param = static_cast<const CreateParam*>(create_param);
    if (!handle || *handle != nullptr) {
      return Fail(
          __func__, error::kInvalidState,
          "Invalid handle argument: handle must be non-null and *handle must "
          "be null");
    }
    *handle = nullptr;

    if (!param) {
      return Fail(__func__, error::kInvalidState, "Null CreateParam pointer");
    }

    if (!param->model_path || param->model_path[0] == '\0') {
      return Fail(__func__, error::kInvalidArgument,
                  "Missing or empty model_path in CreateParam");
    }

    if (!param->cfg_file_name || param->cfg_file_name[0] == '\0') {
      return Fail(__func__, error::kInvalidArgument,
                  "Missing or empty cfg_file_name in CreateParam");
    }

    if (param->device_id < 0) {
      return Fail(__func__, error::kInvalidArgument,
                  "Invalid device_id < 0: " + std::to_string(param->device_id));
    }

    if (!IsSupportedComputePlatform(param->compute_platform)) {
      return Fail(
          __func__, error::kInvalidArgument,
          "Unsupported or unknown ComputePlatform: " +
              std::to_string(static_cast<int>(param->compute_platform)));
    }

    llm_edgeflow::RuntimeCreateOptions options;
    options.model_root = param->model_path;
    options.config_file = param->cfg_file_name;
    options.device_id = param->device_id;
    options.compute_platform = ComputePlatformToString(param->compute_platform);
    options.output_pool_depth = param->max_frame_depth;
    auto handle_instance = std::make_unique<OperatorHandle>();
    std::string error;
    llm_edgeflow::RuntimeFailureStage failure_stage;
    const int result = llm_edgeflow::SharedAlgorithmRuntime::Create(
        options, &handle_instance->runtime, &error, &failure_stage);
    if (result != 0) {
      if (failure_stage == llm_edgeflow::RuntimeFailureStage::kPreparation) {
        return Fail(__func__, result, error,
                    llm_edgeflow::OperatorFailureStage::kCreatePreparation);
      }
      return Fail(__func__, result, error);
    }

    OperatorHandle* raw_h = handle_instance.get();
    OperatorHandleManager::Instance().Register(raw_h);

    *handle = static_cast<void*>(handle_instance.release());
    ALG_LOG_INFO("[Operator] Create completed\n");
    return 0;
  } catch (const std::exception& e) {
    if (handle) *handle = nullptr;
    return Fail(__func__, error::kInternalException, e.what());
  } catch (...) {
    if (handle) *handle = nullptr;
    return Fail(__func__, error::kUnknownException,
                "Unknown exception in Create");
  }
}

int Operator_Process(void* handle, const NamedIoBatch& inputs,
                     NamedIoBatch& outputs) noexcept {
  try {
    if (!handle) {
      return Fail(__func__, error::kInvalidState, "Null handle in Process");
    }

    if (!OperatorHandleManager::Instance().IsValid(handle)) {
      return Fail(__func__, error::kInvalidState,
                  "Invalid or already destroyed handle in Process");
    }

    auto* h = static_cast<OperatorHandle*>(handle);

    if (inputs.empty()) {
      return Fail(__func__, error::kInvalidInput, "Empty inputs NamedIoBatch");
    }

    if (outputs.empty()) {
      return Fail(__func__, error::kInvalidOutput,
                  "Empty outputs NamedIoBatch");
    }

    if (inputs.size() != outputs.size()) {
      return Fail(__func__, error::kInvalidInput,
                  "Mismatched batch size: inputs.size() (" +
                      std::to_string(inputs.size()) + ") != outputs.size() (" +
                      std::to_string(outputs.size()) + ")");
    }

    if (h->runtime && inputs.size() > h->runtime->ProcessBatchLimit()) {
      return Fail(__func__, error::kInvalidInput,
                  "Input batch size " + std::to_string(inputs.size()) +
                      " exceeds effective batch limit " +
                      std::to_string(h->runtime->ProcessBatchLimit()));
    }

    const auto* plan = h->runtime ? h->runtime->GetIoPlan() : nullptr;
    if (!plan || plan->inputs.empty() || plan->outputs.empty()) {
      return Fail(
          __func__, error::kInvalidState,
          "Handle runtime or converter definitions are null in Process");
    }

    std::vector<llm_edgeflow::ExternalInputBatchView> input_views;
    std::string input_error;
    const int input_result = llm_edgeflow::ValidateAndExtractOperatorInputs(
        inputs, plan->inputs, llm_edgeflow::InputLimits{}, &input_views,
        &input_error);
    if (input_result != 0) {
      return Fail(__func__, input_result, input_error);
    }

    std::vector<std::vector<llm_edgeflow::FrameOutputBinding>>
        frame_out_bindings;
    std::string output_error;
    const int output_result = llm_edgeflow::ResolveOperatorOutputs(
        outputs, plan->outputs, &frame_out_bindings, &output_error);
    if (output_result != 0) {
      return Fail(__func__, output_result, output_error);
    }

    llm_edgeflow::RuntimeInputBatch runtime_inputs;
    runtime_inputs.count = inputs.size();
    runtime_inputs.views = std::move(input_views);
    llm_edgeflow::RuntimeOutputBatch runtime_outputs;
    runtime_outputs.rows.resize(outputs.size());
    for (size_t row = 0; row < frame_out_bindings.size(); ++row)
      for (const auto& slot : frame_out_bindings[row])
        runtime_outputs.rows[row].push_back({slot.output_index, {}});
    std::string error;
    llm_edgeflow::RuntimeFailureStage failure_stage;
    const int result = h->runtime->Process(runtime_inputs, &runtime_outputs,
                                           &error, &failure_stage);
    if (result != 0) {
      if (failure_stage == llm_edgeflow::RuntimeFailureStage::kExecution) {
        return Fail(__func__, result, error,
                    llm_edgeflow::OperatorFailureStage::kProcessExecution);
      }
      return Fail(__func__, result, error);
    }
    llm_edgeflow::PublishOperatorOutputs(&runtime_outputs, frame_out_bindings,
                                         plan->outputs, &outputs);
    return 0;
  } catch (const std::exception& e) {
    return Fail(__func__, error::kInternalException, e.what());
  } catch (...) {
    return Fail(__func__, error::kUnknownException,
                "Unknown exception in Process");
  }
}

int Operator_Control(void* handle, int command_value,
                     void* control_param) noexcept {
  try {
    const auto command = static_cast<ControlCommand>(command_value);
    if (!handle) {
      return Fail(__func__, error::kInvalidState, "Null handle in Control");
    }

    if (!OperatorHandleManager::Instance().IsValid(handle)) {
      return Fail(__func__, error::kInvalidState,
                  "Invalid or already destroyed handle in Control");
    }

    auto* h = static_cast<OperatorHandle*>(handle);

    if (!h->runtime) {
      return Fail(__func__, error::kInvalidState,
                  "Handle runtime is null in Control");
    }

    int cmd_id = 0;
    std::string json_str;
    std::string resolve_err;
    int res_ret = llm_edgeflow::OperatorControlRegistry::ResolveControlParam(
        command, control_param, &cmd_id, &json_str, &resolve_err);
    if (res_ret != 0) {
      return Fail(__func__, res_ret,
                  "ResolveControlParam failed: " + resolve_err);
    }

    std::string exec_err;
    llm_edgeflow::ControlFailureStage control_stage =
        llm_edgeflow::ControlFailureStage::kNone;
    const int result = h->runtime->ExecuteControl({cmd_id, std::move(json_str)},
                                                  &exec_err, &control_stage);
    if (result != 0) {
      const auto stage =
          llm_edgeflow::ControlFailureToOperatorStage(control_stage);
      return Fail(__func__, result, exec_err, stage);
    }
    return 0;
  } catch (const std::exception& e) {
    return Fail(__func__, error::kInternalException, e.what());
  } catch (...) {
    return Fail(__func__, error::kUnknownException,
                "Unknown exception in Control");
  }
}

int Operator_Destroy(void* handle) noexcept {
  try {
    if (!handle) {
      return Fail(__func__, error::kInvalidState, "Null handle in Destroy");
    }

    OperatorHandle* h =
        OperatorHandleManager::Instance().ExtractForDestroy(handle);
    if (!h) {
      return Fail(__func__, error::kInvalidState,
                  "Invalid, unmanaged or already destroyed handle in Destroy");
    }

    std::unique_ptr<OperatorHandle> owner(h);
    std::string error;
    const int result = owner->runtime->Close(&error);
    if (result != 0) return Fail(__func__, result, error);
    ALG_LOG_INFO("[Operator] Destroy completed\n");
    return COMPANY_ALG_SUCCESS;
  } catch (const std::exception& e) {
    return Fail(__func__, error::kInternalException, e.what());
  } catch (...) {
    return Fail(__func__, error::kUnknownException,
                "Unknown exception in Destroy");
  }
}

int Operator_DeInit() noexcept {
  try {
    std::string diagnostic;
    int cleanup_ret = OperatorHandleManager::Instance().DestroyAll(&diagnostic);
    if (cleanup_ret != 0) return Fail(__func__, cleanup_ret, diagnostic);
    return COMPANY_ALG_SUCCESS;
  } catch (const std::exception& e) {
    return Fail(__func__, error::kInternalException, e.what());
  } catch (...) {
    return Fail(__func__, error::kUnknownException,
                "Unknown exception in DeInit");
  }
}

}  // namespace

OperatorFunc Get_LLM_EDGEFLOW_OperatorTable() noexcept {
  static const OperatorFunc table{
      Operator_Init,    Operator_Create,  Operator_Control,
      Operator_Process, Operator_Destroy, Operator_DeInit,
  };
  return table;
}

const char* GetOperatorLastError() noexcept {
  return g_last_operator_error.c_str();
}

int ResolveOperatorConfigIo(const char* model_path, const char* cfg_file_name,
                            OperatorIoContract* out, char* out_error_msg,
                            size_t error_buf_size) noexcept {
  try {
    if (!out) {
      if (out_error_msg && error_buf_size > 0)
        std::snprintf(out_error_msg, error_buf_size, "Null out");
      return Fail(__func__, error::kInvalidArgument, "Null out");
    }
    *out = {};
    if (!model_path || model_path[0] == '\0') {
      if (out_error_msg && error_buf_size > 0) {
        std::snprintf(out_error_msg, error_buf_size,
                      "Null or empty model_path");
      }
      return Fail(__func__, error::kInvalidArgument,
                  "Null or empty model_path");
    }
    if (!cfg_file_name || cfg_file_name[0] == '\0') {
      if (out_error_msg && error_buf_size > 0) {
        std::snprintf(out_error_msg, error_buf_size,
                      "Null or empty cfg_file_name");
      }
      return Fail(__func__, error::kInvalidArgument,
                  "Null or empty cfg_file_name");
    }
    llm_edgeflow::RuntimeCreateOptions options;
    options.model_root = model_path;
    options.config_file = cfg_file_name;
    std::unique_ptr<llm_edgeflow::ValidatedIoPlan> resolved;
    std::string error;
    const int result = llm_edgeflow::SharedAlgorithmRuntime::ResolveIoPlan(
        options, &resolved, &error);
    if (result != 0) {
      if (out_error_msg && error_buf_size > 0)
        std::snprintf(out_error_msg, error_buf_size, "%s", error.c_str());
      return Fail(__func__, result, error,
                  OperatorFailureStage::kCreatePreparation);
    }

    OperatorIoContract contract;
    const auto& plan = *resolved;
    for (const auto& selected : plan.inputs) {
      const auto& def = *selected.converter;
      contract.inputs.push_back(
          {def.type, def.name, selected.host_binding.external_c_type_name,
           selected.host_binding.ServiceType(def.name), def.slot.required});
    }
    for (const auto& selected : plan.outputs) {
      const auto& def = *selected.converter;
      contract.outputs.push_back(
          {def.type, def.name, selected.host_binding.external_c_type_name,
           selected.host_binding.ServiceType(def.name), def.slot.required});
    }
    *out = std::move(contract);
    return 0;
  } catch (const std::exception& e) {
    if (out) *out = {};
    if (out_error_msg && error_buf_size > 0) {
      std::snprintf(out_error_msg, error_buf_size, "Exception: %s", e.what());
    }
    return Fail(__func__, error::kInternalException, e.what());
  } catch (...) {
    if (out) *out = {};
    if (out_error_msg && error_buf_size > 0) {
      std::snprintf(out_error_msg, error_buf_size,
                    "Unknown exception in ResolveOperatorConfigIo");
    }
    return Fail(__func__, error::kUnknownException,
                "Unknown exception in ResolveOperatorConfigIo");
  }
}

}  // namespace llm_edgeflow::operator_api

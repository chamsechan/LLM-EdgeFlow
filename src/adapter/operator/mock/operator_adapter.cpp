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
#include "edgeflow/operator/interface.h"

namespace llm_edgeflow::operator_api {

namespace {

thread_local std::string g_last_operator_error;

void SetLastError(std::string_view err) noexcept {
  SetDiagnosticNoexcept(&g_last_operator_error, err);
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

  int DestroyAll() noexcept {
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
        const int result = owner->runtime->Close();
        if (result != 0 && first_error == 0) first_error = result;
      }
      return first_error;
    } catch (...) {
      return -100;
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
    if (result != 0) SetLastError("GlobalInit failed: " + diagnostic);
    return result;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return -99;
  } catch (...) {
    SetLastError("Unknown exception in Init");
    return -100;
  }
}

int Operator_Create(void** handle, const void* create_param) noexcept {
  try {
    const auto* param = static_cast<const CreateParam*>(create_param);
    if (!handle || *handle != nullptr) {
      SetLastError(
          "Invalid handle argument: handle must be non-null and *handle must "
          "be null");
      return -1;
    }
    *handle = nullptr;

    if (!param) {
      SetLastError("Null CreateParam pointer");
      return -1;
    }

    if (!param->model_path || param->model_path[0] == '\0') {
      SetLastError("Missing or empty model_path in CreateParam");
      return -2;
    }

    if (!param->cfg_file_name || param->cfg_file_name[0] == '\0') {
      SetLastError("Missing or empty cfg_file_name in CreateParam");
      return -2;
    }

    if (param->device_id < 0) {
      SetLastError("Invalid device_id < 0: " +
                   std::to_string(param->device_id));
      return -2;
    }

    if (!IsSupportedComputePlatform(param->compute_platform)) {
      SetLastError("Unsupported or unknown ComputePlatform: " +
                   std::to_string(static_cast<int>(param->compute_platform)));
      return -2;
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
        SetLastError(llm_edgeflow::DescribeOperatorFailure(
            llm_edgeflow::OperatorFailureStage::kCreatePreparation, result,
            error));
        return llm_edgeflow::PublicFailureCode(
            llm_edgeflow::OperatorFailureStage::kCreatePreparation, result);
      }
      SetLastError(error);
      return result;
    }

    OperatorHandle* raw_h = handle_instance.get();
    OperatorHandleManager::Instance().Register(raw_h);

    *handle = static_cast<void*>(handle_instance.release());
    return 0;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    if (handle) *handle = nullptr;
    return -99;
  } catch (...) {
    SetLastError("Unknown exception in Create");
    if (handle) *handle = nullptr;
    return -100;
  }
}

int Operator_Process(void* handle, const NamedIoBatch& inputs,
                     NamedIoBatch& outputs) noexcept {
  try {
    if (!handle) {
      SetLastError("Null handle in Process");
      return -1;
    }

    if (!OperatorHandleManager::Instance().IsValid(handle)) {
      SetLastError("Invalid or already destroyed handle in Process");
      return -1;
    }

    auto* h = static_cast<OperatorHandle*>(handle);

    if (inputs.empty()) {
      SetLastError("Empty inputs NamedIoBatch");
      return -3;
    }

    if (outputs.empty()) {
      SetLastError("Empty outputs NamedIoBatch");
      return -4;
    }

    if (inputs.size() != outputs.size()) {
      SetLastError("Mismatched batch size: inputs.size() (" +
                   std::to_string(inputs.size()) + ") != outputs.size() (" +
                   std::to_string(outputs.size()) + ")");
      return -3;
    }

    if (h->runtime && inputs.size() > h->runtime->ProcessBatchLimit()) {
      SetLastError("Input batch size " + std::to_string(inputs.size()) +
                   " exceeds effective batch limit " +
                   std::to_string(h->runtime->ProcessBatchLimit()));
      return -3;
    }

    const auto* plan = h->runtime ? h->runtime->GetIoPlan() : nullptr;
    if (!plan || plan->inputs.empty() || plan->outputs.empty()) {
      SetLastError(
          "Handle runtime or converter definitions are null in Process");
      return -1;
    }

    std::vector<llm_edgeflow::ExternalInputBatchView> input_views;
    std::string input_error;
    const int input_result = llm_edgeflow::ValidateAndExtractOperatorInputs(
        inputs, plan->inputs, llm_edgeflow::InputLimits{}, &input_views,
        &input_error);
    if (input_result != 0) {
      SetLastError(input_error);
      return input_result;
    }

    std::vector<std::vector<llm_edgeflow::FrameOutputBinding>>
        frame_out_bindings;
    std::string output_error;
    const int output_result = llm_edgeflow::ResolveOperatorOutputs(
        outputs, plan->outputs, &frame_out_bindings, &output_error);
    if (output_result != 0) {
      SetLastError(output_error);
      return output_result;
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
        SetLastError(llm_edgeflow::DescribeOperatorFailure(
            llm_edgeflow::OperatorFailureStage::kProcessExecution, result,
            error));
        return llm_edgeflow::PublicFailureCode(
            llm_edgeflow::OperatorFailureStage::kProcessExecution, result);
      }
      SetLastError(error);
      return result;
    }
    llm_edgeflow::PublishOperatorOutputs(&runtime_outputs, frame_out_bindings,
                                         plan->outputs, &outputs);
    return 0;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return -99;
  } catch (...) {
    SetLastError("Unknown exception in Process");
    return -100;
  }
}

int Operator_Control(void* handle, int command_value,
                     void* control_param) noexcept {
  try {
    const auto command = static_cast<ControlCommand>(command_value);
    if (!handle) {
      SetLastError("Null handle in Control");
      return -1;
    }

    if (!OperatorHandleManager::Instance().IsValid(handle)) {
      SetLastError("Invalid or already destroyed handle in Control");
      return -1;
    }

    auto* h = static_cast<OperatorHandle*>(handle);

    if (!h->runtime) {
      SetLastError("Handle runtime is null in Control");
      return -1;
    }

    int cmd_id = 0;
    std::string json_str;
    std::string resolve_err;
    int res_ret = llm_edgeflow::OperatorControlRegistry::ResolveControlParam(
        command, control_param, &cmd_id, &json_str, &resolve_err);
    if (res_ret != 0) {
      SetLastError("ResolveControlParam failed: " + resolve_err);
      return res_ret;
    }

    std::string exec_err;
    llm_edgeflow::ControlFailureStage control_stage =
        llm_edgeflow::ControlFailureStage::kNone;
    const int result = h->runtime->ExecuteControl({cmd_id, std::move(json_str)},
                                                  &exec_err, &control_stage);
    if (result != 0) {
      const auto stage =
          llm_edgeflow::ControlFailureToOperatorStage(control_stage);
      SetLastError(
          llm_edgeflow::DescribeOperatorFailure(stage, result, exec_err));
      return llm_edgeflow::PublicFailureCode(stage, result);
    }
    return 0;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return -99;
  } catch (...) {
    SetLastError("Unknown exception in Control");
    return -100;
  }
}

int Operator_Destroy(void* handle) noexcept {
  try {
    if (!handle) {
      SetLastError("Null handle in Destroy");
      return -1;
    }

    OperatorHandle* h =
        OperatorHandleManager::Instance().ExtractForDestroy(handle);
    if (!h) {
      SetLastError("Invalid, unmanaged or already destroyed handle in Destroy");
      return -1;
    }

    std::unique_ptr<OperatorHandle> owner(h);
    std::string error;
    const int result = owner->runtime->Close(&error);
    if (result != 0) SetLastError(error);
    return result;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return -99;
  } catch (...) {
    SetLastError("Unknown exception in Destroy");
    return -100;
  }
}

int Operator_DeInit() noexcept {
  try {
    int cleanup_ret = OperatorHandleManager::Instance().DestroyAll();
    return cleanup_ret;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return -99;
  } catch (...) {
    SetLastError("Unknown exception in DeInit");
    return -100;
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
      return -2;
    }
    *out = {};
    if (!model_path || model_path[0] == '\0') {
      if (out_error_msg && error_buf_size > 0) {
        std::snprintf(out_error_msg, error_buf_size,
                      "Null or empty model_path");
      }
      return -2;
    }
    if (!cfg_file_name || cfg_file_name[0] == '\0') {
      if (out_error_msg && error_buf_size > 0) {
        std::snprintf(out_error_msg, error_buf_size,
                      "Null or empty cfg_file_name");
      }
      return -2;
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
      return result;
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
    return -99;
  } catch (...) {
    if (out) *out = {};
    if (out_error_msg && error_buf_size > 0) {
      std::snprintf(out_error_msg, error_buf_size,
                    "Unknown exception in ResolveOperatorConfigIo");
    }
    return -100;
  }
}

}  // namespace llm_edgeflow::operator_api

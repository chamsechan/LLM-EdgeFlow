#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "adapter/platform/company_conf_resolver.h"
#include "adapter/platform/platform_control_registry.h"
#include "adapter/platform/platform_io_registry.h"
#include "adapter/shared_algorithm_runtime.h"
#include "platform/platform_operator_interface.h"

namespace llm_edgeflow::platform {

namespace {

thread_local std::string g_last_platform_error;

void SetLastError(const std::string& err) { g_last_platform_error = err; }

struct PlatformHandle {
  std::unique_ptr<alg_framework::SharedAlgorithmRuntime> runtime;
  PlatformConfig platform_config;
  uint32_t depth_num = 1;
  CompanyAlgBizType biz_type = ALG_BIZ_TYPE_UNKNOWN;
  std::mutex mutex;
  bool is_valid = true;
};

int Platform_Init() noexcept {
  try {
    int ret = alg_framework::SharedAlgorithmRuntime::GlobalInit();
    if (ret != 0) {
      SetLastError("GlobalInit failed with registration conflict");
      return ret;
    }
    return 0;
  } catch (const std::exception& e) {
    SetLastError(std::string("Init exception: ") + e.what());
    return -99;
  } catch (...) {
    SetLastError("Unknown exception in Init");
    return -100;
  }
}

int Platform_Create(void** handle, const CreateParam* param) noexcept {
  try {
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

    if (!param->cfg_file_name || param->cfg_file_name[0] == '\0') {
      SetLastError("Missing or empty cfg_file_name in CreateParam");
      return -2;
    }

    if (param->platform_config.batch_size <= 0) {
      SetLastError("Invalid batch_size <= 0: " +
                   std::to_string(param->platform_config.batch_size));
      return -2;
    }

    if (param->platform_config.device_id < 0) {
      SetLastError("Invalid device_id < 0: " +
                   std::to_string(param->platform_config.device_id));
      return -2;
    }

    if (param->platform_config.type == ChipType::kUnknown) {
      SetLastError("Invalid ChipType::kUnknown");
      return -2;
    }

    if (param->depth_num == 0) {
      SetLastError("Invalid depth_num == 0");
      return -2;
    }

    // 1. 解析 .conf 文件并合成 Pipeline JSON
    alg_framework::ResolvedCompanyConfig resolved_conf;
    std::string resolve_err;
    if (!alg_framework::CompanyConfResolver::Resolve(
            param->cfg_file_name, param->platform_config, &resolved_conf,
            &resolve_err)) {
      SetLastError("CompanyConfResolver failed: " + resolve_err);
      return -2;
    }

    // 2. 根据合成的 Pipeline JSON 构建内部共享运行时
    std::unique_ptr<alg_framework::SharedAlgorithmRuntime> runtime;
    std::string create_err;
    int create_ret =
        alg_framework::SharedAlgorithmRuntime::CreateFromPipelineJson(
            resolved_conf.synthetic_pipeline_json,
            param->platform_config.device_id,
            resolved_conf.conf_path.parent_path().string(),
            resolved_conf.biz_type, &runtime, &create_err);
    if (create_ret != 0) {
      SetLastError("SharedAlgorithmRuntime::CreateFromPipelineJson failed: " +
                   create_err);
      return create_ret;
    }

    auto handle_instance = std::make_unique<PlatformHandle>();
    handle_instance->runtime = std::move(runtime);
    handle_instance->platform_config = param->platform_config;
    handle_instance->depth_num = param->depth_num;
    handle_instance->biz_type = resolved_conf.biz_type;
    handle_instance->is_valid = true;

    *handle = static_cast<void*>(handle_instance.release());
    return 0;
  } catch (const std::exception& e) {
    SetLastError(std::string("Create exception: ") + e.what());
    if (handle) *handle = nullptr;
    return -99;
  } catch (...) {
    SetLastError("Unknown exception in Create");
    if (handle) *handle = nullptr;
    return -100;
  }
}

int Platform_Process(void* handle, const NamedIoBatch& inputs,
                     NamedIoBatch& outputs) noexcept {
  try {
    if (!handle) {
      SetLastError("Null handle in Process");
      return -1;
    }

    auto* h = static_cast<PlatformHandle*>(handle);
    if (!h->is_valid || !h->runtime) {
      SetLastError("Invalid or already destroyed handle in Process");
      return -1;
    }

    if (inputs.empty()) {
      SetLastError("Empty inputs NamedIoBatch");
      return -3;
    }

    if (inputs.size() != outputs.size()) {
      SetLastError("Mismatched batch size: inputs.size() (" +
                   std::to_string(inputs.size()) + ") != outputs.size() (" +
                   std::to_string(outputs.size()) + ")");
      return -3;
    }

    if (inputs.size() > static_cast<size_t>(h->platform_config.batch_size)) {
      SetLastError("Input batch size " + std::to_string(inputs.size()) +
                   " exceeds platform configured batch_size " +
                   std::to_string(h->platform_config.batch_size));
      return -3;
    }

    // 同句柄串行化互斥锁保证
    std::lock_guard<std::mutex> lock(h->mutex);

    // 提取并校验输入槽位
    std::vector<const void*> in_ptrs;
    std::string extract_in_err;
    int ext_in_ret =
        alg_framework::PlatformIoRegistry::Instance().ExtractInputs(
            h->biz_type, inputs, &in_ptrs, &extract_in_err);
    if (ext_in_ret != 0) {
      SetLastError("ExtractInputs failed: " + extract_in_err);
      return ext_in_ret;
    }

    // 提取并校验输出槽位
    std::vector<void*> out_ptrs;
    std::string extract_out_err;
    int ext_out_ret =
        alg_framework::PlatformIoRegistry::Instance().ExtractOutputs(
            h->biz_type, outputs, &out_ptrs, &extract_out_err);
    if (ext_out_ret != 0) {
      SetLastError("ExtractOutputs failed: " + extract_out_err);
      return ext_out_ret;
    }

    // 执行计算
    int num_outputs = static_cast<int>(out_ptrs.size());
    std::string exec_err;
    int exec_ret = h->runtime->ExecuteBatch(
        in_ptrs.data(), static_cast<int>(in_ptrs.size()), out_ptrs.data(),
        &num_outputs, &exec_err);
    if (exec_ret != 0) {
      SetLastError("ExecuteBatch failed: " + exec_err);
      return exec_ret;
    }

    return 0;
  } catch (const std::exception& e) {
    SetLastError(std::string("Process exception: ") + e.what());
    return -99;
  } catch (...) {
    SetLastError("Unknown exception in Process");
    return -100;
  }
}

int Platform_Control(void* handle, ControlCommand command,
                     void* control_param) noexcept {
  try {
    if (!handle) {
      SetLastError("Null handle in Control");
      return -1;
    }

    auto* h = static_cast<PlatformHandle*>(handle);
    if (!h->is_valid || !h->runtime) {
      SetLastError("Invalid or already destroyed handle in Control");
      return -1;
    }

    // 同句柄串行化互斥锁保证
    std::lock_guard<std::mutex> lock(h->mutex);

    int cmd_id = 0;
    std::string json_str;
    std::string resolve_err;
    int res_ret = alg_framework::PlatformControlRegistry::ResolveControlParam(
        command, control_param, &cmd_id, &json_str, &resolve_err);
    if (res_ret != 0) {
      SetLastError("ResolveControlParam failed: " + resolve_err);
      return res_ret;
    }

    std::string exec_err;
    int exec_ret = h->runtime->ExecuteControl(cmd_id, json_str, &exec_err);
    if (exec_ret != 0) {
      SetLastError("ExecuteControl failed: " + exec_err);
      return exec_ret;
    }

    return 0;
  } catch (const std::exception& e) {
    SetLastError(std::string("Control exception: ") + e.what());
    return -99;
  } catch (...) {
    SetLastError("Unknown exception in Control");
    return -100;
  }
}

int Platform_Destroy(void* handle) noexcept {
  try {
    if (!handle) {
      SetLastError("Null handle in Destroy");
      return -1;
    }

    auto* h = static_cast<PlatformHandle*>(handle);
    if (!h->is_valid) {
      SetLastError("Double Destroy or invalid handle");
      return -1;
    }

    h->is_valid = false;
    delete h;
    return 0;
  } catch (const std::exception& e) {
    SetLastError(std::string("Destroy exception: ") + e.what());
    return -99;
  } catch (...) {
    SetLastError("Unknown exception in Destroy");
    return -100;
  }
}

int Platform_Deinit() noexcept {
  try {
    return alg_framework::SharedAlgorithmRuntime::GlobalDeinit();
  } catch (const std::exception& e) {
    SetLastError(std::string("Deinit exception: ") + e.what());
    return -99;
  } catch (...) {
    SetLastError("Unknown exception in Deinit");
    return -100;
  }
}

}  // namespace

OperatorFunc Get_LLM_EDGEFLOW_OperatorTable() noexcept {
  static const OperatorFunc table{
      Platform_Init,    Platform_Create,  Platform_Process,
      Platform_Control, Platform_Destroy, Platform_Deinit,
  };
  return table;
}

const char* GetPlatformLastError() noexcept {
  return g_last_platform_error.c_str();
}

}  // namespace llm_edgeflow::platform

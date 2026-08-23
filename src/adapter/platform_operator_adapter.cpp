#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "adapter/platform/company_conf_resolver.h"
#include "adapter/platform/platform_control_registry.h"
#include "adapter/platform/platform_io_registry.h"
#include "adapter/shared_algorithm_runtime.h"
#include "core/session_context.h"
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
  const alg_framework::PlatformIoDescriptor* io_descriptor = nullptr;
  OutputDeallocator output_deallocator = nullptr;
  void* user_data = nullptr;
  std::vector<std::pair<std::string, std::shared_ptr<void>>> pooled_outputs;
  std::mutex mutex;
};

/**
 * @brief 全局线程安全活跃句柄注册中心 (杜绝 Use-After-Free 与悬挂指针解引用)
 */
class PlatformHandleManager {
 public:
  static PlatformHandleManager& Instance() {
    static PlatformHandleManager instance;
    return instance;
  }

  bool Register(PlatformHandle* h) {
    if (!h) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return active_handles_.insert(h).second;
  }

  bool IsValid(void* handle) {
    if (!handle) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return active_handles_.find(static_cast<PlatformHandle*>(handle)) !=
           active_handles_.end();
  }

  PlatformHandle* ExtractForDestroy(void* handle) {
    if (!handle) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_handles_.find(static_cast<PlatformHandle*>(handle));
    if (it == active_handles_.end()) {
      return nullptr;
    }
    PlatformHandle* h = *it;
    active_handles_.erase(it);
    return h;
  }

  /**
   * @brief 安全清理并释放所有活跃句柄资源 (含预分配输出池与底层 Runtime)
   */
  void DestroyAll() {
    std::vector<PlatformHandle*> to_destroy;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto* h : active_handles_) {
        to_destroy.push_back(h);
      }
      active_handles_.clear();
    }
    for (auto* h : to_destroy) {
      if (!h) continue;
      {
        std::lock_guard<std::mutex> lock(h->mutex);
        if (h->output_deallocator) {
          for (auto& item : h->pooled_outputs) {
            h->output_deallocator(item.first.c_str(), std::move(item.second),
                                  h->user_data);
          }
        }
        h->pooled_outputs.clear();
        h->runtime.reset();
      }
      delete h;
    }
  }

 private:
  std::mutex mutex_;
  std::unordered_set<PlatformHandle*> active_handles_;
};

int Platform_Init() noexcept {
  try {
    int ret = alg_framework::SharedAlgorithmRuntime::GlobalInit();
    if (ret != 0) {
      SetLastError(
          "GlobalInit failed: registration conflict detected in registry");
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

    if (!IsSupportedChipType(param->platform_config.type)) {
      SetLastError(
          "Unsupported or unknown ChipType: " +
          std::to_string(static_cast<int>(param->platform_config.type)));
      return -2;
    }

    if (param->depth_num == 0) {
      SetLastError("Invalid depth_num == 0");
      return -2;
    }

    // 1. 解析 .conf 文件并规范化模型绝对路径与合成 Pipeline JSON
    alg_framework::ResolvedCompanyConfig resolved_conf;
    std::string resolve_err;
    int res_code = alg_framework::CompanyConfResolver::Resolve(
        param->cfg_file_name, param->platform_config, &resolved_conf,
        &resolve_err);
    if (res_code != 0) {
      SetLastError("CompanyConfResolver failed: " + resolve_err);
      return res_code;
    }

    // 2. 组装运行时参数 (贯通 ChipType、平台最大 Batch 与 depth_num)
    alg_framework::RuntimeOptions runtime_options;
    runtime_options.chip_type = ChipTypeToString(param->platform_config.type);
    runtime_options.platform_max_batch = param->platform_config.batch_size;
    runtime_options.depth_num = param->depth_num;
    runtime_options.device_id = param->platform_config.device_id;
    runtime_options.has_device_id = (param->platform_config.device_id >= 0);
    runtime_options.biz_type = static_cast<int>(resolved_conf.biz_type);
    runtime_options.business_name = resolved_conf.business_name;

    // 3. 根据合成的 Pipeline JSON 构建内部共享运行时 (模型路径已全量绝对规范化)
    std::unique_ptr<alg_framework::SharedAlgorithmRuntime> runtime;
    std::string create_err;
    int create_ret =
        alg_framework::SharedAlgorithmRuntime::CreateFromPipelineJson(
            resolved_conf.synthetic_pipeline_json,
            param->platform_config.device_id,
            "",  // 传空字符串，模型路径已在 resolved_conf 中全部绝对规范化
            resolved_conf.biz_type, &runtime, &create_err, &runtime_options);
    if (create_ret != 0) {
      SetLastError("SharedAlgorithmRuntime::CreateFromPipelineJson failed: " +
                   create_err);
      return create_ret;
    }

    // 4. (可选) 处理 depth_num 输出对象预分配 Hook
    std::vector<std::pair<std::string, std::shared_ptr<void>>> output_pool;
    if (param->output_allocator && resolved_conf.io_descriptor) {
      try {
        for (uint32_t d = 0; d < param->depth_num; ++d) {
          for (const auto& group : resolved_conf.io_descriptor->output_groups) {
            auto ptr = param->output_allocator(group.canonical_suffix.c_str(),
                                               param->user_data);
            if (!ptr || ptr.get() == nullptr) {
              throw std::runtime_error(
                  "OutputAllocator returned null pointer for slot: " +
                  group.canonical_suffix);
            }
            output_pool.push_back({group.canonical_suffix, std::move(ptr)});
          }
        }
      } catch (const std::exception& e) {
        // 回滚已分配对象
        if (param->output_deallocator) {
          for (auto& item : output_pool) {
            param->output_deallocator(item.first.c_str(),
                                      std::move(item.second), param->user_data);
          }
        }
        SetLastError(std::string("Output memory allocation failed: ") +
                     e.what());
        return -4;
      }
    }

    auto handle_instance = std::make_unique<PlatformHandle>();
    handle_instance->runtime = std::move(runtime);
    handle_instance->platform_config = param->platform_config;
    handle_instance->depth_num = param->depth_num;
    handle_instance->biz_type = resolved_conf.biz_type;
    handle_instance->io_descriptor = resolved_conf.io_descriptor;
    handle_instance->output_deallocator = param->output_deallocator;
    handle_instance->user_data = param->user_data;
    handle_instance->pooled_outputs = std::move(output_pool);

    PlatformHandle* raw_h = handle_instance.get();
    PlatformHandleManager::Instance().Register(raw_h);

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

    if (!PlatformHandleManager::Instance().IsValid(handle)) {
      SetLastError("Invalid or already destroyed handle in Process");
      return -1;
    }

    auto* h = static_cast<PlatformHandle*>(handle);

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

    if (inputs.size() > static_cast<size_t>(h->platform_config.batch_size)) {
      SetLastError("Input batch size " + std::to_string(inputs.size()) +
                   " exceeds platform configured batch_size " +
                   std::to_string(h->platform_config.batch_size));
      return -3;
    }

    // 同句柄串行化互斥锁保证
    std::lock_guard<std::mutex> lock(h->mutex);

    // 二次检查 handle 有效性
    if (!h->runtime) {
      SetLastError("Handle runtime is null in Process");
      return -1;
    }

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

    if (!PlatformHandleManager::Instance().IsValid(handle)) {
      SetLastError("Invalid or already destroyed handle in Control");
      return -1;
    }

    auto* h = static_cast<PlatformHandle*>(handle);

    // 同句柄串行化互斥锁保证
    std::lock_guard<std::mutex> lock(h->mutex);

    if (!h->runtime) {
      SetLastError("Handle runtime is null in Control");
      return -1;
    }

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

    // 原子摘除活跃登记表，杜绝 UAF 与双重释放
    PlatformHandle* h =
        PlatformHandleManager::Instance().ExtractForDestroy(handle);
    if (!h) {
      SetLastError("Invalid, unmanaged or already destroyed handle in Destroy");
      return -1;
    }

    {
      std::lock_guard<std::mutex> lock(h->mutex);
      // 释放池化预分配对象
      if (h->output_deallocator) {
        for (auto& item : h->pooled_outputs) {
          h->output_deallocator(item.first.c_str(), std::move(item.second),
                                h->user_data);
        }
      }
      h->pooled_outputs.clear();
      h->runtime.reset();
    }

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
    PlatformHandleManager::Instance().DestroyAll();
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

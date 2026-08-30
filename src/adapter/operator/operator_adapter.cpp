#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "adapter/operator/company_conf_resolver.h"
#include "adapter/operator/operator_biz_bridge_registry.h"
#include "adapter/operator/operator_control_registry.h"
#include "adapter/operator/operator_output_pool.h"
#include "adapter/operator/operator_process_binding.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "adapter/shared_algorithm_runtime.h"
#include "operator/operator_interface.h"

namespace llm_edgeflow::operator_api {

namespace {

thread_local std::string g_last_operator_error;

void SetLastError(const std::string& err) { g_last_operator_error = err; }

struct OperatorHandle {
  std::unique_ptr<alg_framework::SharedAlgorithmRuntime> runtime;
  uint32_t max_frame_depth = 25;
  uint32_t effective_process_batch_limit = 25;
  CompanyAlgBizType biz_type = ALG_BIZ_TYPE_UNKNOWN;
  const alg_framework::OperatorBizBridgeDescriptor* bridge = nullptr;
  alg_framework::ResolvedCompanyConfig resolved_conf;
  std::unordered_map<std::string,
                     std::shared_ptr<alg_framework::OutputPoolState>>
      output_pools;
  std::mutex mutex;
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
        uint32_t outstanding = 0;
        for (auto& [suffix, pool] : owner->output_pools) {
          if (pool) {
            outstanding += pool->CloseAndDrain();
            pool->DestroyBlocks();
          }
        }
        if (outstanding > 0 && first_error == 0) {
          first_error = -1;
        }
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
    int ret = alg_framework::SharedAlgorithmRuntime::GlobalInit();
    if (ret != 0) {
      SetLastError(
          "GlobalInit failed: registration conflict in SharedAlgorithmRuntime");
      return ret;
    }
    ret = alg_framework::OperatorValueTypeRegistry::Instance().GlobalInit();
    if (ret != 0) {
      SetLastError(
          "GlobalInit failed: registration conflict in "
          "OperatorValueTypeRegistry");
      return ret;
    }
    ret = alg_framework::OperatorBizBridgeRegistry::Instance().GlobalInit();
    if (ret != 0) {
      SetLastError(
          "GlobalInit failed: registration conflict in "
          "OperatorBizBridgeRegistry");
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

int Operator_Create(void** handle, const CreateParam* param) noexcept {
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

    uint32_t effective_depth =
        param->max_frame_depth > 0 ? param->max_frame_depth : 25;
    if (effective_depth > alg_framework::kMaxOutputPoolDepth) {
      SetLastError("max_frame_depth (" + std::to_string(effective_depth) +
                   ") exceeds hard limit " +
                   std::to_string(alg_framework::kMaxOutputPoolDepth));
      return -2;
    }

    // 1. 双路径安全解析 .conf
    alg_framework::ResolvedCompanyConfig resolved_conf;
    std::string resolve_err;
    int res_code = alg_framework::CompanyConfResolver::Resolve(
        param->model_path, param->cfg_file_name, param->device_id,
        param->compute_platform, &resolved_conf, &resolve_err, effective_depth);
    if (res_code != 0) {
      SetLastError("CompanyConfResolver failed: " + resolve_err);
      return res_code;
    }

    // 2. 组装运行时参数
    uint32_t adapter_max_batch = static_cast<uint32_t>(
        resolved_conf.adapter->GetDescriptor().max_batch_size);
    uint32_t effective_batch_limit =
        std::min(effective_depth, adapter_max_batch);

    alg_framework::RuntimeOptions runtime_options;
    runtime_options.chip_type =
        ComputePlatformToString(param->compute_platform);
    runtime_options.platform_max_batch =
        static_cast<int32_t>(effective_batch_limit);
    runtime_options.depth_num = effective_depth;
    runtime_options.device_id = param->device_id;
    runtime_options.has_device_id = (param->device_id >= 0);
    runtime_options.biz_type = static_cast<int>(resolved_conf.biz_type);
    runtime_options.biz_name = resolved_conf.biz_name;

    // 3. 构建内部共享运行时
    std::unique_ptr<alg_framework::SharedAlgorithmRuntime> runtime;
    std::string create_err;
    int create_ret =
        alg_framework::SharedAlgorithmRuntime::CreateFromPipelineJson(
            resolved_conf.synthetic_pipeline_json, param->device_id,
            "",  // 模型路径已全量绝对规范化
            resolved_conf.biz_type, &runtime, &create_err, &runtime_options);
    if (create_ret != 0) {
      SetLastError("SharedAlgorithmRuntime::CreateFromPipelineJson failed: " +
                   create_err);
      return create_ret;
    }

    // 4. 预分配输出内存池
    std::unordered_map<std::string,
                       std::shared_ptr<alg_framework::OutputPoolState>>
        pools;
    for (const auto& out_slot : resolved_conf.bridge_descriptor->output_slots) {
      const auto* binding = alg_framework::OperatorValueTypeRegistry::Instance()
                                .GetBindingBySuffix(out_slot.type_suffix);
      if (!binding) {
        SetLastError("Missing value type binding for output suffix: " +
                     out_slot.type_suffix);
        return -5;
      }
      std::shared_ptr<alg_framework::OutputPoolState> pool;
      std::string pool_err;
      int pool_ret = alg_framework::OutputPoolState::Create(
          out_slot.type_suffix, effective_depth, resolved_conf.output_pool_spec,
          binding, &pool, &pool_err);
      if (pool_ret != 0 || !pool) {
        SetLastError("Failed to create output pool for suffix " +
                     out_slot.type_suffix + ": " + pool_err);
        return pool_ret != 0 ? pool_ret : -4;
      }
      pools[out_slot.type_suffix] = std::move(pool);
    }

    auto handle_instance = std::make_unique<OperatorHandle>();
    handle_instance->runtime = std::move(runtime);
    handle_instance->max_frame_depth = effective_depth;
    handle_instance->effective_process_batch_limit = effective_batch_limit;
    handle_instance->biz_type = resolved_conf.biz_type;
    handle_instance->bridge = resolved_conf.bridge_descriptor;
    handle_instance->resolved_conf = std::move(resolved_conf);
    handle_instance->output_pools = std::move(pools);

    OperatorHandle* raw_h = handle_instance.get();
    OperatorHandleManager::Instance().Register(raw_h);

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

    if (inputs.size() > h->effective_process_batch_limit) {
      SetLastError("Input batch size " + std::to_string(inputs.size()) +
                   " exceeds effective batch limit " +
                   std::to_string(h->effective_process_batch_limit));
      return -3;
    }

    std::lock_guard<std::mutex> lock(h->mutex);

    if (!h->runtime || !h->bridge) {
      SetLastError("Handle runtime or bridge is null in Process");
      return -1;
    }

    size_t batch_size = inputs.size();
    alg_framework::ProcessLocalShadowStorage shadow_storage;
    std::vector<const void*> internal_in_dtos(batch_size, nullptr);
    std::string binding_error;
    int binding_result = alg_framework::ConvertOperatorInputs(
        inputs, *h->bridge, h->resolved_conf.input_limits, &shadow_storage,
        &internal_in_dtos, &binding_error);
    if (binding_result != 0) {
      SetLastError(binding_error);
      return binding_result;
    }

    std::vector<std::vector<alg_framework::FrameOutputBinding>>
        frame_out_bindings;
    binding_result = alg_framework::ResolveOperatorOutputs(
        outputs, *h->bridge, &frame_out_bindings, &binding_error);
    if (binding_result != 0) {
      SetLastError(binding_error);
      return binding_result;
    }

    alg_framework::ScopedOutputLeaseGuard lease_guard;
    std::vector<alg_framework::AcquiredOutputBlock> acquired_blocks;
    binding_result = alg_framework::AcquireOperatorOutputBlocks(
        frame_out_bindings, h->output_pools, &lease_guard, &acquired_blocks,
        &binding_error);
    if (binding_result != 0) {
      SetLastError(binding_error);
      return binding_result;
    }

    // 4. 执行内部 Runtime 计算
    std::vector<void*> internal_out_dtos(batch_size, nullptr);
    for (size_t i = 0; i < batch_size; ++i) {
      if (h->bridge->create_shadow_output_dto) {
        internal_out_dtos[i] =
            h->bridge->create_shadow_output_dto(shadow_storage);
      }
    }
    int num_outputs = static_cast<int>(batch_size);
    std::string exec_err;
    int exec_ret = h->runtime->ExecuteBatch(
        internal_in_dtos.data(), static_cast<int>(batch_size),
        internal_out_dtos.data(), &num_outputs, &exec_err);
    if (exec_ret != 0) {
      SetLastError("ExecuteBatch failed: " + exec_err);
      return exec_ret;
    }
    if (num_outputs != static_cast<int>(batch_size)) {
      SetLastError("ExecuteBatch output count mismatch: expected " +
                   std::to_string(batch_size) + ", got " +
                   std::to_string(num_outputs));
      return -4;
    }

    // 5. 将内部输出结果转换到已租用的池化外部结构中
    for (const auto& acq : acquired_blocks) {
      const void* internal_dto = internal_out_dtos[acq.frame_idx];
      if (!internal_dto) {
        SetLastError("Internal output DTO is null for frame " +
                     std::to_string(acq.frame_idx));
        return -4;
      }
      std::string conv_out_err;
      int conv_ret = h->bridge->convert_sample_output(
          internal_dto, acq.raw_block, acq.pool->Spec(), &conv_out_err);
      if (conv_ret != 0) {
        SetLastError("ConvertSampleOutput failed for key " + acq.key + ": " +
                     conv_out_err);
        return conv_ret;
      }
    }

    alg_framework::PublishOperatorOutputs(acquired_blocks, &outputs,
                                          &lease_guard);
    return 0;
  } catch (const std::exception& e) {
    SetLastError(std::string("Process exception: ") + e.what());
    return -99;
  } catch (...) {
    SetLastError("Unknown exception in Process");
    return -100;
  }
}

int Operator_Control(void* handle, ControlCommand command,
                     void* control_param) noexcept {
  try {
    if (!handle) {
      SetLastError("Null handle in Control");
      return -1;
    }

    if (!OperatorHandleManager::Instance().IsValid(handle)) {
      SetLastError("Invalid or already destroyed handle in Control");
      return -1;
    }

    auto* h = static_cast<OperatorHandle*>(handle);
    std::lock_guard<std::mutex> lock(h->mutex);

    if (!h->runtime) {
      SetLastError("Handle runtime is null in Control");
      return -1;
    }

    int cmd_id = 0;
    std::string json_str;
    std::string resolve_err;
    int res_ret = alg_framework::OperatorControlRegistry::ResolveControlParam(
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
    uint32_t unreturned_count = 0;
    for (auto& [suffix, pool] : owner->output_pools) {
      if (pool) {
        unreturned_count += pool->CloseAndDrain();
        pool->DestroyBlocks();
      }
    }

    if (unreturned_count > 0) {
      SetLastError("Destroy called with " + std::to_string(unreturned_count) +
                   " unreturned output blocks still checked out");
      return -1;
    }

    return 0;
  } catch (const std::exception& e) {
    SetLastError(std::string("Destroy exception: ") + e.what());
    return -99;
  } catch (...) {
    SetLastError("Unknown exception in Destroy");
    return -100;
  }
}

int Operator_Deinit() noexcept {
  try {
    int cleanup_ret = OperatorHandleManager::Instance().DestroyAll();
    int deinit_ret = alg_framework::SharedAlgorithmRuntime::GlobalDeinit();
    return cleanup_ret != 0 ? cleanup_ret : deinit_ret;
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
      Operator_Init,    Operator_Create,  Operator_Process,
      Operator_Control, Operator_Destroy, Operator_Deinit,
  };
  return table;
}

const char* GetOperatorLastError() noexcept {
  return g_last_operator_error.c_str();
}

int ValidateOperatorConfigBinding(const char* model_path,
                                  const char* cfg_file_name,
                                  int32_t expected_biz_type,
                                  char* out_error_msg,
                                  size_t error_buf_size) noexcept {
  if (!model_path || model_path[0] == '\0') {
    if (out_error_msg && error_buf_size > 0) {
      std::snprintf(out_error_msg, error_buf_size, "Null or empty model_path");
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

  alg_framework::ResolvedCompanyConfig resolved;
  std::string err;
  int ret = alg_framework::CompanyConfResolver::Resolve(
      model_path, cfg_file_name, 0, ComputePlatform::kCpu, &resolved, &err);
  if (ret != 0) {
    if (out_error_msg && error_buf_size > 0) {
      std::snprintf(out_error_msg, error_buf_size, "%s", err.c_str());
    }
    return ret;
  }

  if (expected_biz_type != 0 &&
      resolved.biz_type != static_cast<CompanyAlgBizType>(expected_biz_type)) {
    if (out_error_msg && error_buf_size > 0) {
      std::snprintf(
          out_error_msg, error_buf_size,
          "Biz mismatch: Config resolves to biz_type %d, but expected %d",
          static_cast<int>(resolved.biz_type), expected_biz_type);
    }
    return -3;
  }

  return 0;
}

}  // namespace llm_edgeflow::operator_api

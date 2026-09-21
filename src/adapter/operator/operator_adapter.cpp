#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "adapter/operator/operator_config_resolver.h"
#include "adapter/operator/operator_control_registry.h"
#include "adapter/operator/operator_output_pool.h"
#include "adapter/operator/operator_process_binding.h"
#include "adapter/operator/operator_value_type_registry.h"
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
  uint32_t effective_process_batch_limit = 25;
  const llm_edgeflow::InputConverterDefinition* input_converter = nullptr;
  const llm_edgeflow::OutputConverterDefinition* output_converter = nullptr;
  llm_edgeflow::ResolvedInputLimits input_limits;
  std::unordered_map<std::string,
                     std::shared_ptr<llm_edgeflow::OutputPoolState>>
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
    int ret = llm_edgeflow::SharedAlgorithmRuntime::GlobalInit();
    if (ret != 0) {
      SetLastError(
          "GlobalInit failed: registration conflict in SharedAlgorithmRuntime");
      return ret;
    }
    ret = llm_edgeflow::OperatorValueTypeRegistry::Instance().GlobalInit();
    if (ret != 0) {
      SetLastError(
          "GlobalInit failed: registration conflict in "
          "OperatorValueTypeRegistry");
      return ret;
    }
    return 0;
  } catch (const std::exception& e) {
    SetLastError(e.what());
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
    if (effective_depth > llm_edgeflow::kMaxOutputPoolDepth) {
      SetLastError("max_frame_depth (" + std::to_string(effective_depth) +
                   ") exceeds hard limit " +
                   std::to_string(llm_edgeflow::kMaxOutputPoolDepth));
      return -2;
    }

    // 1. 安全解析部署配置 (.conf) 与接入绑定
    llm_edgeflow::ResolvedOperatorConfig resolved_conf;
    std::string resolve_err;
    int res_code = llm_edgeflow::OperatorConfigResolver::Resolve(
        param->model_path, param->cfg_file_name, &resolved_conf, &resolve_err,
        effective_depth);
    if (res_code != 0) {
      SetLastError("OperatorConfigResolver failed: " + resolve_err);
      return res_code;
    }

    // 2. 组装运行时参数
    uint32_t adapter_max_batch =
        static_cast<uint32_t>(resolved_conf.io_plan->effective_max_batch_size);
    uint32_t effective_batch_limit =
        std::min(effective_depth, adapter_max_batch);

    llm_edgeflow::RuntimeOptions runtime_options;
    runtime_options.chip_type =
        ComputePlatformToString(param->compute_platform);
    runtime_options.platform_max_batch =
        static_cast<int32_t>(effective_batch_limit);
    runtime_options.depth_num = effective_depth;
    runtime_options.device_id = param->device_id;
    runtime_options.has_device_id = (param->device_id >= 0);
    runtime_options.biz_name = resolved_conf.biz_name;

    // 3. 构建内部共享运行时 (通过已验证的 IoPlan)
    std::unique_ptr<llm_edgeflow::SharedAlgorithmRuntime> runtime;
    std::string create_err;
    int create_ret = llm_edgeflow::SharedAlgorithmRuntime::CreateFromIoPlan(
        std::move(resolved_conf.io_plan), param->device_id, &runtime_options,
        &runtime, &create_err);
    if (create_ret != 0) {
      SetLastError("SharedAlgorithmRuntime::CreateFromIoPlan failed: " +
                   create_err);
      return create_ret;
    }

    // 4. 预分配输出内存池
    std::unordered_map<std::string,
                       std::shared_ptr<llm_edgeflow::OutputPoolState>>
        pools;
    for (const auto& out_slot :
         runtime->GetIoPlan()->output_converter->external_slots) {
      if (out_slot.direction != llm_edgeflow::PortDirection::kOutput) continue;
      auto pit =
          runtime->GetIoPlan()->operator_output_specs.find(out_slot.slot_name);
      if (pit == runtime->GetIoPlan()->operator_output_specs.end()) {
        if (out_slot.required) {
          SetLastError("Missing output pool configuration for slot " +
                       out_slot.slot_name);
          return -2;
        }
        continue;
      }
      const auto& allocation = pit->second;
      const auto* binding =
          llm_edgeflow::OperatorValueTypeRegistry::Instance().GetOutputBinding(
              out_slot.type_suffix, allocation.allocator);
      if (!binding) {
        SetLastError("Missing output allocator for slot " + out_slot.slot_name +
                     " (type " + out_slot.type_suffix + ")");
        return -5;
      }
      std::shared_ptr<llm_edgeflow::OutputPoolState> pool;
      std::string pool_err;
      int pool_ret = llm_edgeflow::OutputPoolState::Create(
          out_slot.type_suffix, effective_depth, allocation, binding, &pool,
          &pool_err);
      if (pool_ret != 0 || !pool) {
        SetLastError("Failed to create output pool for slot " +
                     out_slot.slot_name + " (type " + out_slot.type_suffix +
                     "): " + pool_err);
        return pool_ret != 0 ? pool_ret : -4;
      }
      pools[out_slot.slot_name] = std::move(pool);
    }

    auto handle_instance = std::make_unique<OperatorHandle>();
    handle_instance->effective_process_batch_limit = effective_batch_limit;
    handle_instance->input_converter = runtime->GetIoPlan()->input_converter;
    handle_instance->output_converter = runtime->GetIoPlan()->output_converter;
    handle_instance->input_limits = resolved_conf.input_limits;
    handle_instance->output_pools = std::move(pools);
    handle_instance->runtime = std::move(runtime);

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

    if (inputs.size() > h->effective_process_batch_limit) {
      SetLastError("Input batch size " + std::to_string(inputs.size()) +
                   " exceeds effective batch limit " +
                   std::to_string(h->effective_process_batch_limit));
      return -3;
    }

    std::lock_guard<std::mutex> lock(h->mutex);

    if (!h->runtime || !h->input_converter || !h->output_converter) {
      SetLastError(
          "Handle runtime or converter definitions are null in Process");
      return -1;
    }

    // 1. 验证并提取外部输入槽
    llm_edgeflow::ExternalInputBatchView in_view;
    std::string in_err;
    int in_ret = llm_edgeflow::ValidateAndExtractOperatorInputs(
        inputs, *h->input_converter, h->input_limits, &in_view, &in_err);
    if (in_ret != 0) {
      SetLastError(in_err);
      return in_ret;
    }

    // 2. 验证并解析输出槽绑定
    std::vector<std::vector<llm_edgeflow::FrameOutputBinding>>
        frame_out_bindings;
    std::string out_err;
    int out_ret = llm_edgeflow::ResolveOperatorOutputs(
        outputs, *h->output_converter, &frame_out_bindings, &out_err);
    if (out_ret != 0) {
      SetLastError(out_err);
      return out_ret;
    }

    // 3. 执行统一输入解码
    // (在租用输出块之前完成业务校验；若校验失败则零输出块被租用)
    llm_edgeflow::AlgContext req_ctx;
    llm_edgeflow::InputDecodeOptions in_options;

    in_options.converter_id = h->input_converter->converter_id;

    llm_edgeflow::AdapterStatus decode_status;
    int decode_ret = h->input_converter->decode_fn(
        in_view, in_options, h->runtime->GetIoPlan()->input_port_bindings,
        &req_ctx, &decode_status);
    if (decode_ret != 0) {
      SetLastError("DecodeInput failed for " +
                   h->input_converter->converter_id + ": " +
                   decode_status.ToString());
      return decode_ret;
    }

    // 4. 租用输出池内存块 (受 ScopedOutputLeaseGuard 保护，失败自动归还)
    llm_edgeflow::ScopedOutputLeaseGuard lease_guard;
    std::vector<llm_edgeflow::AcquiredOutputBlock> acquired_blocks;
    std::string acq_err;
    int acq_ret = llm_edgeflow::AcquireOperatorOutputBlocks(
        frame_out_bindings, h->output_pools, &lease_guard, &acquired_blocks,
        &acq_err);
    if (acq_ret != 0) {
      SetLastError("AcquireOperatorOutputBlocks failed: " + acq_err);
      return acq_ret;
    }

    // 5. 执行 Pipeline 计算
    int exec_ret = h->runtime->GetPipeline()->Execute(&req_ctx);
    if (exec_ret != 0) {
      SetLastError("Pipeline::Execute failed with code " +
                   std::to_string(exec_ret) + ": " + req_ctx.GetErrorMessage());
      return exec_ret;
    }

    // 6. 执行统一输出编码 (将结果写入已租用的外部结构块)
    llm_edgeflow::ExternalOutputBatchView out_view;
    out_view.count = inputs.size();
    for (const auto& slot : h->output_converter->external_slots) {
      if (slot.direction == PortDirection::kOutput) {
        out_view.slot_types[slot.slot_name] = slot.type_id;
        auto pool = h->output_pools.find(slot.slot_name);
        if (pool != h->output_pools.end())
          out_view.pool_specs[slot.slot_name] = &pool->second->Spec();
      }
    }
    for (const auto& acq : acquired_blocks) {
      out_view.leased_slots[acq.logical_name].push_back(acq.raw_block);
    }

    llm_edgeflow::OutputEncodeOptions out_options;

    out_options.converter_id = h->output_converter->converter_id;

    size_t written_count = 0;
    llm_edgeflow::AdapterStatus encode_status;
    int encode_ret = h->output_converter->encode_fn(
        &req_ctx, h->runtime->GetIoPlan()->output_port_bindings, out_options,
        &out_view, &written_count, &encode_status);
    if (encode_ret != 0) {
      SetLastError("EncodeOutput failed for " +
                   h->output_converter->converter_id + ": " +
                   encode_status.ToString());
      return encode_ret;
    }
    if (written_count != inputs.size()) {
      SetLastError("EncodeOutput written count (" +
                   std::to_string(written_count) +
                   ") does not match input count (" +
                   std::to_string(inputs.size()) + ")");
      return -4;
    }

    // 7. 发布输出 (两阶段发布并解除 guard)
    llm_edgeflow::PublishOperatorOutputs(acquired_blocks, &outputs,
                                         &lease_guard);
    return 0;
  } catch (const std::exception& e) {
    SetLastError(e.what());
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
    int res_ret = llm_edgeflow::OperatorControlRegistry::ResolveControlParam(
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
      Operator_Init,    Operator_Create,  Operator_Process,
      Operator_Control, Operator_Destroy, Operator_DeInit,
  };
  return table;
}

const char* GetOperatorLastError() noexcept {
  return g_last_operator_error.c_str();
}

int ValidateOperatorConfigBinding(const char* model_path,
                                  const char* cfg_file_name,
                                  const char* expected_binding_id,
                                  char* out_error_msg,
                                  size_t error_buf_size) noexcept {
  try {
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
    if (!expected_binding_id || expected_binding_id[0] == '\0') {
      if (out_error_msg && error_buf_size > 0) {
        std::snprintf(out_error_msg, error_buf_size,
                      "Null or empty expected_binding_id");
      }
      return -2;
    }

    llm_edgeflow::ResolvedOperatorConfig resolved;
    std::string err;
    int ret = llm_edgeflow::OperatorConfigResolver::Resolve(
        model_path, cfg_file_name, &resolved, &err);
    if (ret != 0) {
      if (out_error_msg && error_buf_size > 0) {
        std::snprintf(out_error_msg, error_buf_size, "%s", err.c_str());
      }
      return ret;
    }

    if (resolved.io_binding != expected_binding_id) {
      if (out_error_msg && error_buf_size > 0) {
        std::snprintf(out_error_msg, error_buf_size,
                      "Binding mismatch: Config resolves to binding '%s', but "
                      "expected '%s'",
                      resolved.io_binding.c_str(), expected_binding_id);
      }
      return -3;
    }

    return 0;
  } catch (const std::exception& e) {
    if (out_error_msg && error_buf_size > 0) {
      std::snprintf(out_error_msg, error_buf_size, "Exception: %s", e.what());
    }
    return -99;
  } catch (...) {
    if (out_error_msg && error_buf_size > 0) {
      std::snprintf(out_error_msg, error_buf_size,
                    "Unknown exception in ValidateOperatorConfigBinding");
    }
    return -100;
  }
}

}  // namespace llm_edgeflow::operator_api

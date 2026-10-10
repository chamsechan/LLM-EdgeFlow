#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "adapter/operator/operator_config_resolver.h"
#include "adapter/operator/operator_control_registry.h"
#include "adapter/operator/operator_error_mapping.h"
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
  std::vector<std::shared_ptr<llm_edgeflow::OutputPoolState>> output_pools;
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
        for (auto& pool : owner->output_pools) {
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
    std::string diagnostic;
    int ret = llm_edgeflow::SharedAlgorithmRuntime::GlobalInit(&diagnostic);
    if (ret != 0) {
      SetLastError("GlobalInit failed: " + diagnostic);
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

    uint32_t effective_depth = param->max_frame_depth > 0
                                   ? param->max_frame_depth
                                   : llm_edgeflow::kDefaultOutputPoolDepth;
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
      SetLastError(llm_edgeflow::DescribeOperatorFailure(
          llm_edgeflow::OperatorFailureStage::kCreatePreparation, res_code,
          "OperatorConfigResolver failed: " + resolve_err));
      return llm_edgeflow::PublicFailureCode(
          llm_edgeflow::OperatorFailureStage::kCreatePreparation, res_code);
    }

    // 2. 组装运行时参数
    uint32_t effective_batch_limit =
        resolved_conf.effective_process_batch_limit;

    llm_edgeflow::RuntimeOptions runtime_options;
    runtime_options.chip_type =
        ComputePlatformToString(param->compute_platform);
    runtime_options.device_id = param->device_id;
    runtime_options.has_device_id = (param->device_id >= 0);

    // 3. 构建内部共享运行时 (通过已验证的 IoPlan)
    std::unique_ptr<llm_edgeflow::SharedAlgorithmRuntime> runtime;
    std::string create_err;
    int create_ret = llm_edgeflow::SharedAlgorithmRuntime::CreateFromIoPlan(
        std::move(resolved_conf.io_plan), param->device_id, &runtime_options,
        &runtime, &create_err);
    if (create_ret != 0) {
      SetLastError(llm_edgeflow::DescribeOperatorFailure(
          llm_edgeflow::OperatorFailureStage::kCreatePreparation, create_ret,
          "SharedAlgorithmRuntime::CreateFromIoPlan failed: " + create_err));
      return llm_edgeflow::PublicFailureCode(
          llm_edgeflow::OperatorFailureStage::kCreatePreparation, create_ret);
    }

    // 4. Each output declaration owns one pool, including optional outputs.
    std::vector<std::shared_ptr<llm_edgeflow::OutputPoolState>> pools;
    for (const auto& selected : runtime->GetIoPlan()->outputs) {
      const auto& def = *selected.converter;
      std::shared_ptr<llm_edgeflow::OutputPoolState> pool;
      std::string pool_error;
      const int result = llm_edgeflow::OutputPoolState::Create(
          def.type, effective_depth, selected.pool_spec,
          &selected.allocator_binding, &pool, &pool_error);
      if (result != 0 || !pool) {
        SetLastError("Failed to create output pool for " + def.Label() + ": " +
                     pool_error);
        return result != 0 ? result : -4;
      }
      pools.push_back(std::move(pool));
    }

    auto handle_instance = std::make_unique<OperatorHandle>();
    handle_instance->effective_process_batch_limit = effective_batch_limit;
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

    const auto* plan = h->runtime ? h->runtime->GetIoPlan() : nullptr;
    if (!plan || plan->inputs.empty() || plan->outputs.empty()) {
      SetLastError(
          "Handle runtime or converter definitions are null in Process");
      return -1;
    }

    std::vector<llm_edgeflow::ExternalInputBatchView> input_views;
    std::vector<uint64_t> request_ids;
    std::string input_error;
    const int input_result = llm_edgeflow::ValidateAndExtractOperatorInputs(
        inputs, plan->inputs, llm_edgeflow::InputLimits{}, &input_views,
        &request_ids, &input_error);
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

    // Decode all requests before leasing any output block.
    llm_edgeflow::AlgContext req_ctx;
    for (size_t index = 0; index < plan->inputs.size(); ++index) {
      const auto& selected = plan->inputs[index];
      const auto& def = *selected.converter;
      std::vector<uint64_t> decoded_ids;
      llm_edgeflow::InputDecodeOptions options;
      options.type = def.type;
      options.name = def.name;
      options.params = selected.params.get();
      options.ports = &selected.ports;
      options.request_ids =
          selected.host_binding.read_request_id ? &decoded_ids : nullptr;
      llm_edgeflow::AdapterStatus status;
      const int result =
          def.decode_fn(input_views[index], options, &req_ctx, &status);
      if (result != 0) {
        SetLastError("DecodeInput failed for " + def.Label() + ": " +
                     status.ToString());
        return result;
      }
      if (selected.host_binding.read_request_id && decoded_ids != request_ids) {
        SetLastError(
            "DecodeInput for " + def.Label() +
            " recorded request ids inconsistent with its input structs");
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }
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
      SetLastError(llm_edgeflow::DescribeOperatorFailure(
          llm_edgeflow::OperatorFailureStage::kProcessExecution, exec_ret,
          req_ctx.GetErrorMessage()));
      return llm_edgeflow::PublicFailureCode(
          llm_edgeflow::OperatorFailureStage::kProcessExecution, exec_ret);
    }

    // Every output view retains batch row positions, even when optional keys
    // are absent.
    for (size_t index = 0; index < plan->outputs.size(); ++index) {
      const auto& selected = plan->outputs[index];
      const auto& def = *selected.converter;
      llm_edgeflow::ExternalOutputBatchView view;
      view.count = inputs.size();
      view.required = def.slot.required;
      view.slot_types[def.type] = def.slot.type_id;
      view.pool_specs[def.type] = &h->output_pools[index]->Spec();
      auto& blocks = view.leased_slots[def.type];
      blocks.resize(inputs.size(), nullptr);
      size_t requested_count = 0;
      for (const auto& acquired : acquired_blocks) {
        if (acquired.output_index == index) {
          blocks[acquired.frame_idx] = acquired.raw_block;
          ++requested_count;
        }
      }
      if (requested_count == 0) continue;
      llm_edgeflow::OutputEncodeOptions options;
      options.type = def.type;
      options.name = def.name;
      options.params = selected.params.get();
      options.ports = &selected.ports;
      options.request_ids = &request_ids;
      size_t written_count = 0;
      llm_edgeflow::AdapterStatus status;
      const int result =
          def.encode_fn(&req_ctx, options, &view, &written_count, &status);
      if (result != 0) {
        SetLastError("EncodeOutput failed for " + def.Label() + ": " +
                     status.ToString());
        return result;
      }
      if (written_count != requested_count) {
        SetLastError("EncodeOutput for " + def.Label() + " wrote " +
                     std::to_string(written_count) + " rows; expected " +
                     std::to_string(requested_count));
        return -4;
      }
      if (def.service_type) {
        for (auto* block : blocks) {
          if (block)
            selected.host_binding.write_service_type(block, *def.service_type);
        }
      }
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
    llm_edgeflow::ControlFailureStage control_stage =
        llm_edgeflow::ControlFailureStage::kNone;
    int exec_ret =
        h->runtime->ExecuteControl(cmd_id, json_str, &exec_err, &control_stage);
    if (exec_ret != 0) {
      const auto stage =
          llm_edgeflow::ControlFailureToOperatorStage(control_stage);
      SetLastError(
          llm_edgeflow::DescribeOperatorFailure(stage, exec_ret, exec_err));
      return llm_edgeflow::PublicFailureCode(stage, exec_ret);
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
    for (auto& pool : owner->output_pools) {
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

    OperatorIoContract contract;
    const auto& plan = *resolved.io_plan;
    for (const auto& selected : plan.inputs) {
      const auto& def = *selected.converter;
      contract.inputs.push_back({def.type, def.name, def.slot.type_id,
                                 def.service_type, def.slot.required});
    }
    for (const auto& selected : plan.outputs) {
      const auto& def = *selected.converter;
      contract.outputs.push_back({def.type, def.name, def.slot.type_id,
                                  def.service_type, def.slot.required});
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

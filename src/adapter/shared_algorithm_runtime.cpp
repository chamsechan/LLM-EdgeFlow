#include "adapter/shared_algorithm_runtime.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "adapter/io_converter_registry.h"
#include "adapter/io_plan_resolver.h"
#include "adapter/operator/operator_config_resolver.h"
#include "adapter/operator/operator_output_pool.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "contracts/diagnostic.h"
#include "core/alg_context.h"
#include "core/diagnostic_code.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/session_context.h"
#include "edgeflow/log.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {
namespace {

template <typename Registry>
void CollectConflicts(const char* name, const Registry& registry,
                      std::vector<std::string>* errors) {
  if (!registry.HasConflict()) return;
  for (const auto& message : registry.GetConflictErrors())
    errors->push_back(std::string(name) + ": " + message);
}

int Fail(int code, std::string* error, std::string_view message) noexcept {
  SetDiagnosticNoexcept(error, message);
  return code;
}

}  // namespace

SharedAlgorithmRuntime::SharedAlgorithmRuntime() = default;
SharedAlgorithmRuntime::~SharedAlgorithmRuntime() { Close(); }

int SharedAlgorithmRuntime::GlobalInit(std::string* diagnostic) noexcept {
  try {
    if (diagnostic) diagnostic->clear();
    std::vector<std::string> errors;
    CollectConflicts("NodeRegistry", NodeRegistry::Instance(), &errors);
    CollectConflicts("ModelRegistry", ModelRegistry::Instance(), &errors);
    CollectConflicts("BackendRegistry", BackendRegistry::Instance(), &errors);
    std::vector<std::string> model_errors;
    if (!ModelRegistry::Instance().Audit(BackendRegistry::Instance(),
                                         &model_errors))
      for (auto& error : model_errors)
        errors.push_back("Model audit: " + std::move(error));
    if (OperatorValueTypeRegistry::Instance().HasConflict())
      errors.push_back("OperatorValueTypeRegistry: registration conflict");
    std::vector<std::string> converter_errors;
    if (!IoConverterRegistry::Instance().Audit(&converter_errors))
      for (auto& error : converter_errors)
        errors.push_back("Converter audit: " + std::move(error));
    if (!errors.empty()) {
      std::string message;
      ALG_LOG_ERROR("[SharedAlgorithmRuntime] GlobalInit failed:\n");
      for (const auto& error : errors) {
        ALG_LOG_ERROR("  - %s\n", error.c_str());
        if (!message.empty()) message += "; ";
        message += error;
      }
      return Fail(COMPANY_ALG_ERR_REGISTRY_CONFLICT, diagnostic, message);
    }
    const int result = OperatorValueTypeRegistry::Instance().GlobalInit();
    if (result != 0)
      return Fail(result, diagnostic,
                  "Value type registry initialization failed");
    return COMPANY_ALG_SUCCESS;
  } catch (const std::exception& e) {
    return Fail(COMPANY_ALG_ERR_EXCEPTION, diagnostic, e.what());
  } catch (...) {
    return Fail(COMPANY_ALG_ERR_UNKNOWN, diagnostic,
                "Unknown exception in GlobalInit");
  }
}

int SharedAlgorithmRuntime::ResolveIoPlan(
    const RuntimeCreateOptions& options,
    std::unique_ptr<ValidatedIoPlan>* out_plan,
    std::string* out_error) noexcept {
  try {
    if (!out_plan) return Fail(-2, out_error, "Null plan destination");
    out_plan->reset();
    ResolvedOperatorConfig config;
    const int result = OperatorConfigResolver::Resolve(
        options.model_root.c_str(), options.config_file.c_str(), &config,
        out_error, options.output_pool_depth);
    if (result != 0) return result;
    *out_plan = std::move(config.io_plan);
    return 0;
  } catch (const std::exception& e) {
    return Fail(-99, out_error, e.what());
  } catch (...) {
    return Fail(-100, out_error, "Unknown exception resolving runtime plan");
  }
}

int SharedAlgorithmRuntime::Create(
    const RuntimeCreateOptions& options,
    std::unique_ptr<SharedAlgorithmRuntime>* out_runtime,
    std::string* out_error, RuntimeFailureStage* failure_stage) noexcept {
  if (failure_stage) *failure_stage = RuntimeFailureStage::kPreparation;
  try {
    if (!out_runtime) return Fail(-1, out_error, "Null runtime destination");
    out_runtime->reset();
    std::unique_ptr<ValidatedIoPlan> plan;
    const int result = ResolveIoPlan(options, &plan, out_error);
    if (result != 0) return result;
    return CreateFromIoPlan(std::move(plan), options, out_runtime, out_error,
                            failure_stage);
  } catch (const std::exception& e) {
    return Fail(-99, out_error, e.what());
  } catch (...) {
    return Fail(-100, out_error, "Unknown exception in runtime creation");
  }
}

int SharedAlgorithmRuntime::CreateFromIoPlan(
    std::unique_ptr<ValidatedIoPlan> io_plan,
    const RuntimeCreateOptions& options,
    std::unique_ptr<SharedAlgorithmRuntime>* out_runtime,
    std::string* out_error, RuntimeFailureStage* failure_stage) noexcept {
  if (failure_stage) *failure_stage = RuntimeFailureStage::kPreparation;
  try {
    if (!out_runtime) return Fail(-1, out_error, "Null runtime destination");
    out_runtime->reset();
    if (!io_plan || io_plan->inputs.empty() || io_plan->outputs.empty() ||
        !io_plan->pipeline_plan)
      return Fail(-2, out_error, "Invalid or incomplete ValidatedIoPlan");
    const uint32_t depth = options.output_pool_depth > 0
                               ? options.output_pool_depth
                               : kDefaultOutputPoolDepth;
    if (options.device_id < 0 || depth > kMaxOutputPoolDepth)
      return Fail(-2, out_error, "Invalid runtime device or output pool depth");
    if (depth != io_plan->output_pool_depth)
      return Fail(-2, out_error,
                  "Output pool depth differs from the validated plan budget");

    auto runtime = std::make_unique<SharedAlgorithmRuntime>();
    auto pipeline = std::make_unique<Pipeline>();
    RuntimeOptions runtime_options;
    runtime_options.chip_type = options.compute_platform;
    runtime_options.device_id = options.device_id;
    runtime_options.has_device_id = true;
    pipeline->GetSessionContext().SetRuntimeOptions(runtime_options);
    PipelineDiagnostic diagnostic;
    if (!pipeline->BuildFromPlan(std::move(io_plan->pipeline_plan),
                                 &diagnostic)) {
      const int code = diagnostic.code == DiagnosticCode::kRegistryConflict
                           ? COMPANY_ALG_ERR_REGISTRY_CONFLICT
                           : COMPANY_ALG_ERR_INVALID_INPUT;
      return Fail(
          code, out_error,
          "Failed to build pipeline from plan: " + diagnostic.message +
              " (code: " + std::string(DiagnosticCodeName(diagnostic.code)) +
              ", path: " + diagnostic.path + ")");
    }
    // 输出池错误保留原有返回码；协议壳仅将配置解析和构建失败
    // 按 Create 准备阶段的规则映射。
    if (failure_stage) *failure_stage = RuntimeFailureStage::kNone;
    for (const auto& selected : io_plan->outputs) {
      std::shared_ptr<OutputPoolState> pool;
      std::string error;
      const int result = OutputPoolState::Create(
          selected.converter->type, depth, selected.pool_spec,
          &selected.allocator_binding, &pool, &error);
      if (result != 0 || !pool)
        return Fail(result ? result : -4, out_error,
                    "Failed to create output pool for " +
                        selected.converter->Label() + ": " + error);
      runtime->output_pools_.push_back(std::move(pool));
    }
    runtime->process_batch_limit_ =
        std::min<size_t>(depth, kMaxProcessBatchSize);
    runtime->io_plan_ = std::move(io_plan);
    runtime->pipeline_ = std::move(pipeline);
    *out_runtime = std::move(runtime);
    return 0;
  } catch (const std::exception& e) {
    return Fail(-99, out_error, e.what());
  } catch (...) {
    return Fail(-100, out_error, "Unknown exception in runtime creation");
  }
}

int SharedAlgorithmRuntime::Process(
    const RuntimeInputBatch& inputs, RuntimeOutputBatch* outputs,
    std::string* out_error, RuntimeFailureStage* failure_stage) noexcept {
  if (failure_stage) *failure_stage = RuntimeFailureStage::kNone;
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pipeline_ || !io_plan_)
      return Fail(-1, out_error, "Runtime is closed or uninitialized");
    const auto& plan = *io_plan_;
    if (inputs.count > process_batch_limit_)
      return Fail(-3, out_error,
                  "Input batch size " + std::to_string(inputs.count) +
                      " exceeds effective batch limit " +
                      std::to_string(process_batch_limit_));
    if (!inputs.count || inputs.views.size() != plan.inputs.size())
      return Fail(-3, out_error,
                  "Invalid input batch size or converter view count");
    if (!outputs || outputs->rows.size() != inputs.count)
      return Fail(-4, out_error, "Output row count must equal input row count");
    for (size_t index = 0; index < inputs.views.size(); ++index) {
      const auto& view = inputs.views[index];
      const auto& binding = plan.inputs[index].host_binding;
      if (view.count != inputs.count || !view.binding ||
          view.binding->value_type != binding.value_type ||
          view.binding->external_c_type_name != binding.external_c_type_name)
        return Fail(-3, out_error,
                    "Input view does not match validated selection");
    }
    for (const auto& row : outputs->rows) {
      std::vector<bool> seen(plan.outputs.size(), false);
      for (const auto& slot : row) {
        if (slot.output_index >= seen.size() || seen[slot.output_index] ||
            slot.value)
          return Fail(-4, out_error,
                      "Invalid, duplicate or occupied output slot");
        seen[slot.output_index] = true;
      }
      for (size_t index = 0; index < seen.size(); ++index)
        if (plan.outputs[index].converter->slot.required && !seen[index])
          return Fail(-4, out_error,
                      "Missing required output " +
                          plan.outputs[index].converter->Label());
    }

    AlgContext context;
    for (size_t index = 0; index < plan.inputs.size(); ++index) {
      const auto& selected = plan.inputs[index];
      const auto& def = *selected.converter;
      InputDecodeOptions options;
      options.type = def.type;
      options.name = def.name;
      options.params = selected.params.get();
      options.ports = &selected.ports;
      AdapterStatus status;
      auto view = inputs.views[index];
      // 使用已验证计划中的校验和转换回调，不采用调用方提供的回调。
      view.binding = &selected.host_binding;
      const int result = def.decode_fn(view, options, &context, &status);
      if (result != 0)
        return Fail(
            result, out_error,
            "DecodeInput failed for " + def.Label() + ": " + status.ToString());
    }

    ScopedOutputLeaseGuard guard;
    std::vector<AcquiredOutputBlock> acquired;
    std::string error;
    const int acquired_result = AcquireRuntimeOutputBlocks(
        *outputs, output_pools_, &guard, &acquired, &error);
    if (acquired_result != 0)
      return Fail(acquired_result, out_error,
                  "AcquireRuntimeOutputBlocks failed: " + error);
    const int execution_result = pipeline_->Execute(&context);
    if (execution_result != 0) {
      if (failure_stage) *failure_stage = RuntimeFailureStage::kExecution;
      return Fail(execution_result, out_error, context.GetErrorMessage());
    }

    for (size_t index = 0; index < plan.outputs.size(); ++index) {
      const auto& selected = plan.outputs[index];
      const auto& def = *selected.converter;
      ExternalOutputBatchView view;
      view.count = inputs.count;
      view.required = def.slot.required;
      view.binding = &selected.allocator_binding;
      view.slot_types[def.type] = selected.host_binding.external_c_type_name;
      view.pool_specs[def.type] = &output_pools_[index]->Spec();
      auto& blocks = view.leased_slots[def.type];
      blocks.resize(inputs.count, nullptr);
      size_t requested_count = 0;
      for (const auto& block : acquired) {
        if (block.output_index != index) continue;
        blocks[block.frame_idx] = block.raw_block;
        ++requested_count;
      }
      if (!requested_count) continue;
      OutputEncodeOptions options;
      options.type = def.type;
      options.name = def.name;
      options.params = selected.params.get();
      options.ports = &selected.ports;
      AdapterStatus status;
      size_t written = 0;
      const int result =
          def.encode_fn(&context, options, &view, &written, &status);
      if (result != 0)
        return Fail(result, out_error,
                    "EncodeOutput failed for " + def.Label() + ": " +
                        status.ToString());
      if (written != requested_count)
        return Fail(-4, out_error,
                    "EncodeOutput for " + def.Label() + " wrote " +
                        std::to_string(written) + " rows; expected " +
                        std::to_string(requested_count));
    }
    PublishRuntimeOutputs(acquired, outputs, &guard);
    return 0;
  } catch (const std::exception& e) {
    return Fail(-99, out_error, e.what());
  } catch (...) {
    return Fail(-100, out_error, "Unknown exception in runtime processing");
  }
}

int SharedAlgorithmRuntime::ExecuteControl(
    const ControlRequest& request, std::string* out_error,
    ControlFailureStage* failure_stage) noexcept {
  if (failure_stage) *failure_stage = ControlFailureStage::kNone;
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pipeline_)
      return Fail(-1, out_error, "Runtime is closed or uninitialized");
    return pipeline_->Control(request.command, request.json, out_error,
                              failure_stage);
  } catch (const std::exception& e) {
    if (failure_stage) *failure_stage = ControlFailureStage::kNone;
    return Fail(-99, out_error, e.what());
  } catch (...) {
    if (failure_stage) *failure_stage = ControlFailureStage::kNone;
    return Fail(-100, out_error, "Unknown exception in runtime control");
  }
}

int SharedAlgorithmRuntime::Close(std::string* out_error) noexcept {
  // 调用方须先等待 Process/Control 结束再调用 Close。此处不等待执行互斥锁，
  // 因为 Acquire 可能正阻塞等待调用方归还此前的租约。
  try {
    uint64_t outstanding = 0;
    for (const auto& pool : output_pools_) {
      outstanding += pool->CloseAndDrain();
      pool->DestroyBlocks();
    }
    output_pools_.clear();
    pipeline_.reset();
    io_plan_.reset();
    if (outstanding)
      return Fail(-1, out_error,
                  "Runtime closed with " + std::to_string(outstanding) +
                      " unreturned output blocks still checked out");
    return 0;
  } catch (const std::exception& e) {
    return Fail(-99, out_error, e.what());
  } catch (...) {
    return Fail(-100, out_error, "Unknown exception closing runtime");
  }
}

}  // namespace llm_edgeflow

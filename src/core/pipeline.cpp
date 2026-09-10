#include "core/pipeline.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <fstream>
#include <future>

#include "contracts/control_payload.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"
#include "edgeflow/log.h"
#include "engine/model_runtime_factory.h"

namespace llm_edgeflow {

namespace {

PipelineErrorCode ValidationCodeToPipelineCode(DiagnosticCode code) {
  switch (code) {
    case DiagnosticCode::kOk:
      return PipelineErrorCode::kOk;
    case DiagnosticCode::kJsonParse:
      return PipelineErrorCode::kJsonParse;
    case DiagnosticCode::kConfigFileOpen:
      return PipelineErrorCode::kConfigFileOpen;
    case DiagnosticCode::kRootType:
      return PipelineErrorCode::kRootType;
    case DiagnosticCode::kUnknownField:
    case DiagnosticCode::kUnknownConfigField:
      return PipelineErrorCode::kUnknownField;
    case DiagnosticCode::kMissingField:
    case DiagnosticCode::kMissingConfigField:
      return PipelineErrorCode::kMissingField;
    case DiagnosticCode::kFieldType:
    case DiagnosticCode::kConfigFieldType:
      return PipelineErrorCode::kFieldType;
    case DiagnosticCode::kFieldRange:
    case DiagnosticCode::kConfigFieldRange:
      return PipelineErrorCode::kFieldRange;
    case DiagnosticCode::kInvalidCombination:
    case DiagnosticCode::kConfigFieldEnum:
    case DiagnosticCode::kUnknownBiz:
    case DiagnosticCode::kUnknownModelReference:
    case DiagnosticCode::kModelCapabilityMismatch:
    case DiagnosticCode::kNodeBizMismatch:
    case DiagnosticCode::kMissingInputProducer:
    case DiagnosticCode::kDuplicatePortProducer:
    case DiagnosticCode::kMissingBizOutput:
    case DiagnosticCode::kNodeNotParallelSafe:
    case DiagnosticCode::kParallelWriteConflict:
    case DiagnosticCode::kSerializedModelConcurrency:
    case DiagnosticCode::kPortCardinalityMismatch:
    case DiagnosticCode::kPortProvenanceMismatch:
    case DiagnosticCode::kPortLifetimeMismatch:
      return PipelineErrorCode::kInvalidCombination;
    case DiagnosticCode::kDuplicateModelId:
      return PipelineErrorCode::kDuplicateModelId;
    case DiagnosticCode::kDuplicateNodeId:
      return PipelineErrorCode::kDuplicateNodeId;
    case DiagnosticCode::kUnknownNodeType:
      return PipelineErrorCode::kUnknownNodeType;
    case DiagnosticCode::kUnknownModelType:
      return PipelineErrorCode::kUnknownModelType;
    case DiagnosticCode::kUnknownBackend:
      return PipelineErrorCode::kUnknownBackend;
    case DiagnosticCode::kBackendProtocolMismatch:
      return PipelineErrorCode::kInvalidCombination;
    case DiagnosticCode::kUnknownModelConfigField:
    case DiagnosticCode::kUnknownBackendConfigField:
      return PipelineErrorCode::kUnknownField;
    case DiagnosticCode::kInvalidDependency:
    case DiagnosticCode::kDuplicateDependency:
      return PipelineErrorCode::kInvalidDependency;
    case DiagnosticCode::kDagCycle:
      return PipelineErrorCode::kDagCycle;
    case DiagnosticCode::kRegistryConflict:
      return PipelineErrorCode::kRegistryConflict;
    case DiagnosticCode::kInternalException:
      return PipelineErrorCode::kInternalException;
  }
  return PipelineErrorCode::kInvalidCombination;
}

struct RuntimeAssembly {
  std::unique_ptr<ValidatedPipelinePlan> plan;
  std::unique_ptr<SessionContext> session;
  Pipeline::ExecutionMode execution_mode = Pipeline::ExecutionMode::kSequential;
  size_t max_parallel_workers = 4;
  std::vector<std::unique_ptr<INode>> nodes;
  std::vector<std::vector<INode*>> node_layers;
  std::unique_ptr<ThreadPool> thread_pool;
};

bool MaterializeModels(const ValidatedPipelinePlan& plan,
                       SessionContext* session,
                       PipelineDiagnostic* diagnostic) {
  std::vector<ModelRegistration> staged_models;
  staged_models.reserve(plan.models.size());

  for (const auto& model_plan : plan.models) {
    ModelLoadSpec spec;
    spec.model_type = model_plan.model_type;
    spec.backend_type = model_plan.backend;
    spec.model_path = model_plan.resolved_model_path;
    spec.model_config = model_plan.normalized_model_config;
    spec.backend_config = model_plan.normalized_backend_config;
    const auto& runtime_options = session->GetRuntimeOptions();
    if (runtime_options.has_device_id) {
      spec.execution_target.device_id = runtime_options.device_id;
    }
    spec.execution_target.platform = runtime_options.chip_type;

    std::string factory_diag;
    auto model = ModelRuntimeFactory::Create(spec, &factory_diag);
    if (!model) {
      if (diagnostic) {
        diagnostic->code = PipelineErrorCode::kModelMaterializationFailed;
        diagnostic->path = "/models/" + std::to_string(model_plan.source_index);
        diagnostic->message =
            "ModelRuntimeFactory failed to load model: " + model_plan.model_id +
            (factory_diag.empty() ? "" : (" (" + factory_diag + ")"));
      }
      ALG_LOG_ERROR("[Pipeline] Failed to load model [%s]: %s\n",
                    model_plan.model_id.c_str(), factory_diag.c_str());
      return false;
    }

    ModelRegistration registration;
    registration.model_id = model_plan.model_id;
    registration.model_type = model_plan.model_type;
    registration.capability = model_plan.capability;
    registration.backend_type = model_plan.backend;
    registration.resolved_model_path = model_plan.resolved_model_path;
    registration.normalized_model_config = model_plan.normalized_model_config;
    registration.normalized_backend_config =
        model_plan.normalized_backend_config;
    registration.model = std::move(model);
    staged_models.push_back(std::move(registration));
  }

  if (!session->GetModelManager().RegisterBatch(staged_models)) {
    if (diagnostic) {
      diagnostic->code = PipelineErrorCode::kDuplicateModelId;
      diagnostic->path = "/models";
      diagnostic->message =
          "Failed to atomically register batch models in ModelManager";
    }
    return false;
  }
  return true;
}

void ConfigureExecutor(const ParsedPipelineConfig& config,
                       RuntimeAssembly* assembly) {
  if (config.execution_mode == "parallel") {
    assembly->execution_mode = Pipeline::ExecutionMode::kParallel;
    assembly->max_parallel_workers = config.max_parallel_workers;
    assembly->thread_pool =
        std::make_unique<ThreadPool>(assembly->max_parallel_workers);
    ALG_LOG_INFO(
        "[Pipeline] Parallel Wavefront Execution Mode enabled (workers: %zu)\n",
        assembly->max_parallel_workers);
    return;
  }

  assembly->execution_mode = Pipeline::ExecutionMode::kSequential;
  assembly->thread_pool.reset();
  ALG_LOG_INFO("[Pipeline] Sequential Execution Mode active\n");
}

bool MaterializeNodes(RuntimeAssembly* assembly,
                      PipelineDiagnostic* diagnostic) {
  const auto& plan = *assembly->plan;
  for (size_t layer_index = 0; layer_index < plan.topological_layers.size();
       ++layer_index) {
    std::vector<INode*> layer_nodes;
    for (const auto& node_id : plan.topological_layers[layer_index]) {
      auto plan_it = plan.node_plans.find(node_id);
      if (plan_it == plan.node_plans.end()) {
        if (diagnostic) {
          diagnostic->code = PipelineErrorCode::kInternalException;
          diagnostic->path = "/pipeline";
          diagnostic->message =
              "Validated plan is missing node materialization data: " + node_id;
        }
        return false;
      }

      const auto& node_plan = plan_it->second;
      const auto& node_config = node_plan.node;
      std::unique_ptr<INode> node;
      try {
        node = NodeRegistry::Instance().Create(node_config.node_type);
      } catch (const std::exception& e) {
        if (diagnostic) {
          diagnostic->code = PipelineErrorCode::kNodeCreateFailed;
          diagnostic->path = "/pipeline/" +
                             std::to_string(node_config.source_index) +
                             "/node_type";
          diagnostic->message = "Exception creating node '" +
                                node_config.node_type + "': " + e.what();
        }
        return false;
      } catch (...) {
        if (diagnostic) {
          diagnostic->code = PipelineErrorCode::kNodeCreateFailed;
          diagnostic->path = "/pipeline/" +
                             std::to_string(node_config.source_index) +
                             "/node_type";
          diagnostic->message =
              "Unknown exception creating node '" + node_config.node_type + "'";
        }
        return false;
      }

      if (!node) {
        if (diagnostic) {
          diagnostic->code = PipelineErrorCode::kNodeCreateFailed;
          diagnostic->path = "/pipeline/" +
                             std::to_string(node_config.source_index) +
                             "/node_type";
          diagnostic->message = "NodeRegistry returned null for node_type: " +
                                node_config.node_type;
        }
        ALG_LOG_ERROR("[Pipeline] Failed to create node: %s\n",
                      node_config.node_type.c_str());
        return false;
      }

      bool init_ok = false;
      std::string init_error;
      try {
        NodeInitContext init_ctx;
        init_ctx.plan = &node_plan;
        init_ctx.config = &node_plan.normalized_config;
        init_ctx.session_ctx = assembly->session.get();
        init_ctx.diagnostic = &init_error;
        init_ok = node->Init(init_ctx);
      } catch (const std::exception& e) {
        if (diagnostic) {
          diagnostic->code = PipelineErrorCode::kNodeInitFailed;
          diagnostic->path = "/pipeline/" +
                             std::to_string(node_config.source_index) +
                             "/config";
          diagnostic->message = "Exception initializing node '" +
                                node_config.node_type + "': " + e.what();
        }
        return false;
      } catch (...) {
        if (diagnostic) {
          diagnostic->code = PipelineErrorCode::kNodeInitFailed;
          diagnostic->path = "/pipeline/" +
                             std::to_string(node_config.source_index) +
                             "/config";
          diagnostic->message = "Unknown exception initializing node '" +
                                node_config.node_type + "'";
        }
        return false;
      }

      if (!init_ok) {
        if (diagnostic) {
          diagnostic->code = PipelineErrorCode::kNodeInitFailed;
          diagnostic->path = "/pipeline/" +
                             std::to_string(node_config.source_index) +
                             "/config";
          diagnostic->message = "Failed to initialize node '" +
                                node_config.node_type +
                                "' (id: " + node_config.id + ")";
          if (!init_error.empty()) diagnostic->message += ": " + init_error;
        }
        ALG_LOG_ERROR("[Pipeline] Failed to initialize node: %s (id: %s)\n",
                      node_config.node_type.c_str(), node_config.id.c_str());
        return false;
      }

      layer_nodes.push_back(node.get());
      assembly->nodes.push_back(std::move(node));
      ALG_LOG_DEBUG("[Pipeline] Initialized node [%s] (id: %s, layer: %zu)\n",
                    node_config.node_type.c_str(), node_config.id.c_str(),
                    layer_index);
    }
    assembly->node_layers.push_back(std::move(layer_nodes));
  }

  ALG_LOG_DEBUG(
      "[Pipeline] DAG Wavefront Topology created with %zu execution layers:\n",
      assembly->node_layers.size());
  for (size_t i = 0; i < plan.topological_layers.size(); ++i) {
    std::string node_ids;
    for (size_t j = 0; j < plan.topological_layers[i].size(); ++j) {
      node_ids += plan.topological_layers[i][j];
      if (j + 1 < plan.topological_layers[i].size()) node_ids += ", ";
    }
    ALG_LOG_DEBUG(
        "  Layer %zu [%s]: %s\n", i,
        assembly->node_layers[i].size() > 1 ? "Parallel" : "Sequential",
        node_ids.c_str());
  }
  return true;
}

}  // namespace

Pipeline::NodeExecutionResult Pipeline::ExecuteNodeSafely(
    INode* node, AlgContext* req_ctx, std::string_view node_id) {
  if (!node) return {-1, "Null node pointer in pipeline execution"};
  if (!req_ctx) return {-1, "Null context in pipeline execution"};
  const auto failure = [&](int code, const std::string& message) {
    return NodeExecutionResult{code, "Node '" + std::string(node_id) + "' (" +
                                         node->Name() + "): " + message};
  };
  (void)req_ctx->TakeCurrentThreadError();
  try {
    const int code = node->Process(req_ctx);
    auto error = req_ctx->TakeCurrentThreadError();
    if (code == 0) {
      return {0, {}};
    }
    std::string msg = std::move(error.message);
    if (msg.empty()) {
      msg = "Node '" + node->Name() + "' failed with exit code " +
            std::to_string(code);
    }
    return failure(code, msg);
  } catch (const std::exception& e) {
    (void)req_ctx->TakeCurrentThreadError();
    return failure(-1, std::string("Unhandled Process exception: ") + e.what());
  } catch (...) {
    (void)req_ctx->TakeCurrentThreadError();
    return failure(-1, "Unknown Process exception");
  }
}

Pipeline::Pipeline()
    : session_ctx_(std::make_unique<SessionContext>()),
      plan_(std::make_unique<ValidatedPipelinePlan>()) {}

bool Pipeline::BuildFromConfigFile(const std::string& config_file_path,
                                   PipelineDiagnostic* diagnostic,
                                   ValidationPolicy policy) {
  if (diagnostic) {
    diagnostic->Clear();
  }

  // R1-ACC-002: 一次性构建状态检查
  if (state_ != State::kEmpty) {
    if (diagnostic) {
      diagnostic->code = PipelineErrorCode::kInvalidBuildState;
      diagnostic->path = "/";
      diagnostic->message =
          "Pipeline build can only be attempted once on an empty Pipeline "
          "instance";
    }
    ALG_LOG_ERROR(
        "[Pipeline] Build attempted on non-empty Pipeline (state: %d)\n",
        static_cast<int>(state_));
    return false;
  }

  std::ifstream ifs(config_file_path);
  if (!ifs.is_open()) {
    state_ = State::kFailed;
    if (diagnostic) {
      diagnostic->code = PipelineErrorCode::kConfigFileOpen;
      diagnostic->path = "/";
      diagnostic->message = "Failed to open config file: " + config_file_path;
    }
    ALG_LOG_ERROR("[Pipeline] Failed to open config file: %s\n",
                  config_file_path.c_str());
    return false;
  }

  // R1-ACC-001: 缩小 JSON 解析 try-catch 范围，避免掩盖下游构建异常
  nlohmann::json root_json;
  try {
    ifs >> root_json;
  } catch (const std::exception& e) {
    state_ = State::kFailed;
    if (diagnostic) {
      diagnostic->code = PipelineErrorCode::kJsonParse;
      diagnostic->path = "/";
      diagnostic->message = std::string("JSON parse exception in ") +
                            config_file_path + ": " + e.what();
    }
    ALG_LOG_ERROR("[Pipeline] JSON parse exception in %s: %s\n",
                  config_file_path.c_str(), e.what());
    return false;
  }

  return BuildFromJson(root_json, diagnostic, policy);
}

bool Pipeline::BuildFromJson(const nlohmann::json& root_config,
                             PipelineDiagnostic* diagnostic,
                             ValidationPolicy policy) {
  if (diagnostic) {
    diagnostic->Clear();
  }

  // R1-ACC-002: 一次性构建状态检查
  if (state_ != State::kEmpty) {
    if (diagnostic) {
      diagnostic->code = PipelineErrorCode::kInvalidBuildState;
      diagnostic->path = "/";
      diagnostic->message =
          "Pipeline build can only be attempted once on an empty Pipeline "
          "instance";
    }
    ALG_LOG_ERROR(
        "[Pipeline] Build attempted on non-empty Pipeline (state: %d)\n",
        static_cast<int>(state_));
    return false;
  }

  state_ = State::kBuilding;

  // RECHECK-R1-001: RAII Guard 保证任何未捕获异常退出时状态机必转入
  // kFailed，不滞留在 kBuilding
  struct BuildingStateGuard {
    State& s;
    bool finalized = false;
    ~BuildingStateGuard() {
      if (!finalized) {
        s = State::kFailed;
      }
    }
  } guard{state_};

  bool success = false;
  try {
    success = BuildInternal(root_config, diagnostic, policy);
  } catch (const std::exception& e) {
    success = false;
    if (diagnostic) {
      diagnostic->code = PipelineErrorCode::kInternalException;
      diagnostic->path = "/";
      diagnostic->message =
          std::string("Internal exception during pipeline build: ") + e.what();
    }
    ALG_LOG_ERROR(
        "[Pipeline] Unhandled internal exception during pipeline build: %s\n",
        e.what());
  } catch (...) {
    success = false;
    if (diagnostic) {
      diagnostic->code = PipelineErrorCode::kInternalException;
      diagnostic->path = "/";
      diagnostic->message = "Unknown internal exception during pipeline build";
    }
    ALG_LOG_ERROR(
        "[Pipeline] Unknown internal exception during pipeline build\n");
  }

  state_ = success ? State::kReady : State::kFailed;
  guard.finalized = true;
  return success;
}

bool Pipeline::BuildInternal(const nlohmann::json& root_config,
                             PipelineDiagnostic* diagnostic,
                             ValidationPolicy policy) {
  // FINAL-R1-003: 仅在测试场景下注入异常，以提供 kInternalException
  // 动态覆盖证据
  if (test_internal_hook_) {
    test_internal_hook_();
  }

  RuntimeAssembly assembly;
  assembly.plan = std::make_unique<ValidatedPipelinePlan>(
      PipelineValidator::ValidateAndPlan(root_config, policy));

  if (!assembly.plan->report.ok) {
    if (!assembly.plan->report.diagnostics.empty()) {
      const auto& item = assembly.plan->report.diagnostics.front();
      const char* code_str = DiagnosticCodeName(item.code);
      if (diagnostic) {
        diagnostic->code = ValidationCodeToPipelineCode(item.code);
        diagnostic->path = item.path;
        diagnostic->message = std::string(code_str) + ": " + item.message;
      }
      ALG_LOG_ERROR("[Pipeline] Validation failed: %s at %s: %s\n", code_str,
                    item.path.c_str(), item.message.c_str());
    }
    return false;
  }

  assembly.session = std::make_unique<SessionContext>();
  assembly.session->SetRuntimeOptions(session_ctx_->GetRuntimeOptions());

  if (!MaterializeModels(*assembly.plan, assembly.session.get(), diagnostic)) {
    return false;
  }
  if (!MaterializeNodes(&assembly, diagnostic)) {
    return false;
  }
  ConfigureExecutor(assembly.plan->config, &assembly);

  // The pointed-to Plan and Session objects keep the same addresses across
  // this ownership transfer, so pointers retained by initialized Nodes stay
  // valid. No Pipeline runtime state is published before this point.
  plan_ = std::move(assembly.plan);
  session_ctx_ = std::move(assembly.session);
  execution_mode_ = assembly.execution_mode;
  max_parallel_workers_ = assembly.max_parallel_workers;
  nodes_ = std::move(assembly.nodes);
  node_layers_ = std::move(assembly.node_layers);
  thread_pool_ = std::move(assembly.thread_pool);
  return true;
}

int Pipeline::Execute(AlgContext* req_ctx) {
  // R1-ACC-002: 仅允许在 Ready 状态下执行
  if (state_ != State::kReady || !req_ctx) {
    return -1;
  }

  for (size_t layer_idx = 0; layer_idx < node_layers_.size(); ++layer_idx) {
    const auto& layer = node_layers_[layer_idx];
    if (layer.empty()) continue;

    // 单节点层 或 顺序执行模式：直接主线程执行 (零线程切换开销)
    if (layer.size() == 1 || execution_mode_ == ExecutionMode::kSequential ||
        !thread_pool_) {
      for (size_t i = 0; i < layer.size(); ++i) {
        auto* node = layer[i];
        NodeExecutionResult result = ExecuteNodeSafely(
            node, req_ctx, plan_->topological_layers[layer_idx][i]);
        if (result.code != 0) {
          req_ctx->SetError(result.code, result.message);
          ALG_LOG_ERROR(
              "[Pipeline] Node [%s] failed with error code: %d, msg: %s\n",
              node->Name().c_str(), result.code, result.message.c_str());
          return result.code;
        }
      }
    } else {
      // 多节点并发层：每个任务都有异常屏障，且所有已提交任务都会在
      // Execute 返回前完成，避免后台任务继续访问调用方持有的 req_ctx。
      std::vector<std::future<NodeExecutionResult>> futures;
      futures.reserve(layer.size());
      struct SubmittedNodesGuard {
        std::vector<std::future<NodeExecutionResult>>& futures;
        ~SubmittedNodesGuard() {
          // Diagnostic construction can also throw. Never let submitted tasks
          // retain the caller's context after Execute has unwound.
          for (auto& future : futures) {
            if (future.valid()) future.wait();
          }
        }
      } submitted_nodes_guard{futures};

      std::string submission_error;
      for (size_t i = 0; i < layer.size(); ++i) {
        auto* node = layer[i];
        try {
          const std::string_view node_id =
              plan_->topological_layers[layer_idx][i];
          futures.push_back(thread_pool_->Submit([node, req_ctx, node_id]() {
            return ExecuteNodeSafely(node, req_ctx, node_id);
          }));
        } catch (const std::exception& e) {
          submission_error = "Failed to submit parallel node '" + node->Name() +
                             "': " + e.what();
          break;
        } catch (...) {
          submission_error = "Failed to submit parallel node '" + node->Name() +
                             "': unknown exception";
          break;
        }
      }

      int first_error = 0;
      std::string first_error_message;
      std::string first_error_node;
      for (size_t i = 0; i < futures.size(); ++i) {
        NodeExecutionResult result;
        try {
          result = futures[i].get();
        } catch (const std::exception& e) {
          result = {-1,
                    std::string("Failed to collect parallel node result: ") +
                        e.what()};
        } catch (...) {
          result = {-1,
                    "Failed to collect parallel node result: unknown "
                    "exception"};
        }

        if (result.code != 0 && first_error == 0) {
          first_error = result.code;
          first_error_message = std::move(result.message);
          first_error_node = layer[i]->Name();
        }
      }

      if (first_error == 0 && !submission_error.empty()) {
        first_error = -1;
        first_error_message = std::move(submission_error);
        first_error_node = "executor";
      }
      if (first_error != 0) {
        if (first_error_message.empty()) {
          first_error_message = "Parallel node execution failed";
        }
        req_ctx->SetError(first_error, first_error_message);
        ALG_LOG_ERROR(
            "[Pipeline] Parallel Node [%s] failed with error code: %d, "
            "msg: %s\n",
            first_error_node.c_str(), first_error, first_error_message.c_str());
        return first_error;
      }
    }
  }

  return 0;
}

int Pipeline::Control(int cmd, const std::string& json_param,
                      std::string* error) {
  if (error) error->clear();
  const auto fail = [&](int code, const std::string& message) {
    if (error) *error = message;
    ALG_LOG_ERROR("[Pipeline] %s\n", message.c_str());
    return code;
  };
  if (state_ != State::kReady) {
    return fail(-1, "Control requires a Ready Pipeline");
  }

  struct Target {
    INode* node;
    std::string context;
  };
  std::vector<Target> targets;
  nlohmann::json payload = nlohmann::json::parse(json_param, nullptr, false);
  bool parsed = !payload.is_discarded();
  std::string target_id;
  std::string node_param = json_param;
  if (parsed && payload.is_object() && payload.contains("$edgeflow_control")) {
    static const nlohmann::json envelope_schema = {
        {"type", "object"},
        {"required", {"$edgeflow_control", "node_id", "payload"}},
        {"additionalProperties", false},
        {"properties",
         {{"$edgeflow_control", {{"type", "integer"}, {"enum", {1}}}},
          {"node_id", {{"type", "string"}}},
          {"payload", {{"type", "object"}}}}}};
    std::string detail;
    if (!ValidateControlPayload(payload, envelope_schema, &detail))
      return fail(-1, "Invalid targeted Control envelope: " + detail);
    target_id = payload["node_id"].get<std::string>();
    if (target_id.empty())
      return fail(-1, "Targeted Control node_id must not be empty");
    auto business_payload = payload["payload"];
    payload = std::move(business_payload);
    node_param = payload.dump();
  }
  bool target_found = false;
  size_t node_index = 0;
  // Materialization uses this same layer/instance order. Keep diagnostics tied
  // to instance IDs even when several instances have the same Node type.
  for (const auto& layer : plan_->topological_layers) {
    for (const auto& id : layer) {
      auto* node = nodes_[node_index++].get();
      if (!target_id.empty() && id != target_id) continue;
      target_found = true;
      const auto def = PipelineCatalog::FindNode(node->Name());
      if (!def) continue;
      const auto command = std::find_if(
          def->control_commands.begin(), def->control_commands.end(),
          [&](const auto& item) { return item.cmd_id == cmd; });
      if (command == def->control_commands.end()) continue;
      const std::string context = "Control " + std::to_string(cmd) +
                                  ", node '" + id + "' (" + node->Name() +
                                  "): ";
      if (!command->payload_schema.empty() &&
          command->payload_schema.is_object()) {
        std::string detail;
        const bool valid =
            parsed ? ValidateControlPayload(payload, command->payload_schema,
                                            &detail)
                   : ParseControlPayload(node_param, command->payload_schema,
                                         &payload, &detail);
        if (!valid) return fail(-1, context + detail);
        parsed = true;
      }
      targets.push_back({node, context});
    }
  }
  if (!target_id.empty() && !target_found) {
    return fail(-1, "Unknown Control target node_id: '" + target_id + "'");
  }
  if (targets.empty()) {
    return fail(-7,
                "Unsupported control command: " + std::to_string(cmd) +
                    (target_id.empty() ? "" : " for node '" + target_id + "'"));
  }

  // Broadcast remains best-effort: a later semantic failure does not undo an
  // earlier update. Report each failure instead of hiding it behind an int.
  bool handled = false;
  int first_failure = 0;
  std::string failures;
  for (const auto& target : targets) {
    const auto result = target.node->Control(cmd, node_param);
    if (result.status == NodeControlStatus::kFailed) {
      if (first_failure == 0) first_failure = result.code ? result.code : -1;
      if (!failures.empty()) failures += "; ";
      failures +=
          target.context + (result.message.empty() ? "Node rejected the update"
                                                   : result.message);
    } else if (result.status == NodeControlStatus::kHandled) {
      handled = true;
    }
  }
  if (first_failure != 0) return fail(first_failure, failures);
  if (handled) return 0;
  return fail(
      -7, "Declared control command was not handled: " + std::to_string(cmd));
}

}  // namespace llm_edgeflow

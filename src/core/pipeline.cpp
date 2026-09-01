#include "core/pipeline.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <fstream>
#include <future>

#include "company_alg_log.h"
#include "core/node_registry.h"
#include "core/pipeline_validator.h"
#include "engine/model_runtime_factory.h"

namespace alg_framework {

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
  Pipeline::ExecutionMode execution_mode = Pipeline::ExecutionMode::SEQUENTIAL;
  size_t max_parallel_workers = 4;
  std::vector<std::unique_ptr<INode>> nodes;
  std::vector<std::vector<INode*>> node_layers;
  std::unique_ptr<ThreadPool> thread_pool;
};

struct NodeExecutionResult {
  int code = 0;
  std::string message;
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
    assembly->execution_mode = Pipeline::ExecutionMode::PARALLEL;
    assembly->max_parallel_workers = config.max_parallel_workers;
    assembly->thread_pool =
        std::make_unique<ThreadPool>(assembly->max_parallel_workers);
    ALG_LOG_INFO(
        "[Pipeline] Parallel Wavefront Execution Mode enabled (workers: %zu)\n",
        assembly->max_parallel_workers);
    return;
  }

  assembly->execution_mode = Pipeline::ExecutionMode::SEQUENTIAL;
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
        node = NodeFactory::Instance().Create(node_config.node_type);
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
          diagnostic->message = "NodeFactory returned null for node_type: " +
                                node_config.node_type;
        }
        ALG_LOG_ERROR("[Pipeline] Failed to create node: %s\n",
                      node_config.node_type.c_str());
        return false;
      }

      bool init_ok = false;
      try {
        NodeInitContext init_ctx;
        init_ctx.plan = &node_plan;
        init_ctx.config = &node_plan.normalized_config;
        init_ctx.session_ctx = assembly->session.get();
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
    if (layer.size() == 1 || execution_mode_ == ExecutionMode::SEQUENTIAL ||
        !thread_pool_) {
      for (auto* node : layer) {
        int ret = node->Process(req_ctx);
        if (ret != 0) {
          ALG_LOG_ERROR(
              "[Pipeline] Node [%s] failed with error code: %d, msg: %s\n",
              node->Name().c_str(), ret, req_ctx->GetErrorMessage().c_str());
          return ret;
        }
      }
    } else {
      // 多节点并发层：每个任务都有异常屏障，且所有已提交任务都会在
      // Execute 返回前完成，避免后台任务继续访问调用方持有的 req_ctx。
      std::vector<std::future<NodeExecutionResult>> futures;
      futures.reserve(layer.size());

      std::string submission_error;
      for (size_t i = 0; i < layer.size(); ++i) {
        auto* node = layer[i];
        try {
          futures.push_back(thread_pool_->Submit([node, req_ctx]() {
            (void)req_ctx->TakeCurrentThreadError();
            try {
              const int code = node->Process(req_ctx);
              auto error = req_ctx->TakeCurrentThreadError();
              return NodeExecutionResult{
                  code, code == 0 ? std::string{} : std::move(error.message)};
            } catch (const std::exception& e) {
              (void)req_ctx->TakeCurrentThreadError();
              return NodeExecutionResult{
                  -1, std::string("Unhandled exception in node Process: ") +
                          e.what()};
            } catch (...) {
              (void)req_ctx->TakeCurrentThreadError();
              return NodeExecutionResult{-1,
                                         "Unknown exception in node Process"};
            }
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

namespace {

bool ValidatePayloadSchema(const nlohmann::json& payload,
                           const nlohmann::json& schema, std::string* err_msg) {
  if (schema.empty() || !schema.is_object()) return true;

  const auto fail = [&](const std::string& message) {
    if (err_msg) *err_msg = message;
    return false;
  };

  if (schema.contains("type") && schema["type"].is_string()) {
    const std::string type = schema["type"].get<std::string>();
    const bool matches = (type == "object" && payload.is_object()) ||
                         (type == "array" && payload.is_array()) ||
                         (type == "string" && payload.is_string()) ||
                         (type == "boolean" && payload.is_boolean()) ||
                         (type == "number" && payload.is_number()) ||
                         (type == "integer" && payload.is_number_integer()) ||
                         (type == "null" && payload.is_null());
    if (!matches)
      return fail("Control payload does not match type '" + type + "'");
  }

  if (schema.contains("enum") && schema["enum"].is_array() &&
      std::find(schema["enum"].begin(), schema["enum"].end(), payload) ==
          schema["enum"].end()) {
    return fail("Control payload value is not in the allowed enum");
  }

  if (payload.is_object()) {
    if (schema.contains("minProperties") &&
        schema["minProperties"].is_number_integer()) {
      const auto minimum = schema["minProperties"].get<int64_t>();
      if (minimum >= 0 && payload.size() < static_cast<size_t>(minimum)) {
        return fail("Control payload has fewer properties than required");
      }
    }

    if (schema.contains("required") && schema["required"].is_array()) {
      for (const auto& req : schema["required"]) {
        if (req.is_string()) {
          std::string req_key = req.get<std::string>();
          if (!payload.contains(req_key)) {
            return fail("Missing required field in control payload: " +
                        req_key);
          }
        }
      }
    }

    const nlohmann::json empty_properties = nlohmann::json::object();
    const auto& properties =
        schema.contains("properties") && schema["properties"].is_object()
            ? schema["properties"]
            : empty_properties;
    for (auto it = payload.begin(); it != payload.end(); ++it) {
      if (properties.contains(it.key())) {
        std::string nested_error;
        if (!ValidatePayloadSchema(it.value(), properties[it.key()],
                                   &nested_error)) {
          return fail("Property '" + it.key() + "': " + nested_error);
        }
      } else if (schema.contains("additionalProperties")) {
        const auto& additional = schema["additionalProperties"];
        if (additional.is_boolean() && !additional.get<bool>()) {
          return fail("Unknown property in control payload: " + it.key());
        }
        if (additional.is_object()) {
          std::string nested_error;
          if (!ValidatePayloadSchema(it.value(), additional, &nested_error)) {
            return fail("Property '" + it.key() + "': " + nested_error);
          }
        }
      }
    }
  }

  if (payload.is_array() && schema.contains("items") &&
      schema["items"].is_object()) {
    for (size_t i = 0; i < payload.size(); ++i) {
      std::string nested_error;
      if (!ValidatePayloadSchema(payload[i], schema["items"], &nested_error)) {
        return fail("Array item " + std::to_string(i) + ": " + nested_error);
      }
    }
  }
  return true;
}

}  // namespace

int Pipeline::Control(int cmd, const std::string& json_param) {
  // R1-ACC-002: 仅允许在 Ready 状态下执行
  if (state_ != State::kReady) {
    return -1;
  }

  ALG_LOG_DEBUG("[Pipeline] Control cmd received: %d, params: %s\n", cmd,
                json_param.c_str());

  bool has_target = false;
  bool has_handled = false;
  int first_fail_code = 0;

  // Pass 1: Collect targets and validate payload across all targets
  std::vector<std::pair<INode*, const ControlCommandDefinition*>> targets;
  nlohmann::json parsed_payload;
  bool json_parsed = false;

  for (auto& node : nodes_) {
    const auto* def = PipelineCatalog::FindNode(node->Name());
    const ControlCommandDefinition* matched_cmd_def = nullptr;
    if (def) {
      for (const auto& cmd_def : def->control_commands) {
        if (cmd_def.cmd_id == cmd) {
          matched_cmd_def = &cmd_def;
          break;
        }
      }
    }
    if (!matched_cmd_def) {
      continue;
    }

    if (!matched_cmd_def->payload_schema.empty() &&
        matched_cmd_def->payload_schema.is_object()) {
      if (!json_parsed) {
        try {
          parsed_payload = nlohmann::json::parse(json_param);
          json_parsed = true;
        } catch (const std::exception& e) {
          ALG_LOG_ERROR("[Pipeline] Control payload JSON parse error: %s\n",
                        e.what());
          return -1;
        }
      }
      std::string schema_err;
      if (!ValidatePayloadSchema(
              parsed_payload, matched_cmd_def->payload_schema, &schema_err)) {
        ALG_LOG_ERROR("[Pipeline] Control payload schema violation: %s\n",
                      schema_err.c_str());
        return -1;
      }
    }
    targets.emplace_back(node.get(), matched_cmd_def);
  }

  if (targets.empty()) {
    ALG_LOG_ERROR("[Pipeline] Unsupported control command: %d\n", cmd);
    return -7;  // COMPANY_ALG_ERR_UNSUPPORTED_CONTROL
  }

  // Pass 2: Dispatch command to all validated targets
  for (auto& [node, cmd_def] : targets) {
    has_target = true;
    NodeControlResult res = node->Control(cmd, json_param);
    if (res.status == NodeControlStatus::kFailed) {
      ALG_LOG_ERROR(
          "[Pipeline] Node [%s] Control failed with code: %d, msg: %s\n",
          node->Name().c_str(), res.code, res.message.c_str());
      if (first_fail_code == 0) {
        first_fail_code = res.code != 0 ? res.code : -1;
      }
    } else if (res.status == NodeControlStatus::kHandled) {
      has_handled = true;
    }
  }

  if (first_fail_code != 0) {
    return first_fail_code;
  }
  if (has_handled) {
    return 0;
  }
  if (!has_target) {
    ALG_LOG_ERROR("[Pipeline] Unsupported control command: %d\n", cmd);
    return -7;  // COMPANY_ALG_ERR_UNSUPPORTED_CONTROL
  }
  return -7;
}

}  // namespace alg_framework

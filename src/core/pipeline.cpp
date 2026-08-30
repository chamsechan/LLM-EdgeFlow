#include "core/pipeline.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <future>
#include <queue>
#include <unordered_map>
#include <unordered_set>

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
    case DiagnosticCode::kUnknownBusiness:
    case DiagnosticCode::kUnknownModelReference:
    case DiagnosticCode::kModelCapabilityMismatch:
    case DiagnosticCode::kNodeBusinessMismatch:
    case DiagnosticCode::kMissingInputProducer:
    case DiagnosticCode::kDuplicatePortProducer:
    case DiagnosticCode::kMissingBusinessOutput:
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

}  // namespace

Pipeline::Pipeline() = default;

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

  // 唯一单趟校验与执行计划生成 (Single-Pass Validate and Plan)
  plan_ = PipelineValidator::ValidateAndPlan(
      root_config, policy, session_ctx_.GetRuntimeOptions().model_root_dir);

  if (!plan_.report.ok) {
    if (!plan_.report.diagnostics.empty()) {
      const auto& item = plan_.report.diagnostics.front();
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

  const auto& parsed_cfg = plan_.config;

  // 1. 加载配置中声明的所有模型：先在局部 staging 向量完成
  // Backend/Model 创建，仅当全部成功后原子提交到 ModelManager。
  std::vector<ModelRegistration> staged_new_models;
  staged_new_models.reserve(plan_.models.size());

  // 按 ValidatedPipelinePlan.models 批量物化并暂存。
  for (const auto& model_plan : plan_.models) {
    ModelLoadSpec spec;
    spec.model_type = model_plan.model_type;
    spec.backend_type = model_plan.backend;
    spec.model_path = model_plan.resolved_model_path;
    spec.model_config = model_plan.normalized_model_config;
    spec.backend_config = model_plan.normalized_backend_config;

    std::string factory_diag;
    std::shared_ptr<IModel> model;
    try {
      model = ModelRuntimeFactory::Create(spec, &factory_diag);
    } catch (const std::exception& e) {
      if (diagnostic) {
        diagnostic->code = PipelineErrorCode::kModelMaterializationFailed;
        diagnostic->path = "/models/" + std::to_string(model_plan.source_index);
        diagnostic->message = "Exception creating model '" +
                              model_plan.model_id + "': " + e.what();
      }
      return false;
    } catch (...) {
      if (diagnostic) {
        diagnostic->code = PipelineErrorCode::kModelMaterializationFailed;
        diagnostic->path = "/models/" + std::to_string(model_plan.source_index);
        diagnostic->message =
            "Unknown exception creating model '" + model_plan.model_id + "'";
      }
      return false;
    }

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

    ModelRegistration reg;
    reg.model_id = model_plan.model_id;
    reg.model_type = model_plan.model_type;
    reg.capability = model_plan.capability;
    reg.backend_type = model_plan.backend;
    reg.resolved_model_path = model_plan.resolved_model_path;
    reg.normalized_model_config = model_plan.normalized_model_config;
    reg.normalized_backend_config = model_plan.normalized_backend_config;
    reg.model = std::move(model);
    staged_new_models.push_back(std::move(reg));
  }

  // 统一单锁原子提交到 Session ModelManager。
  if (!session_ctx_.GetModelManager().RegisterBatch(staged_new_models)) {
    if (diagnostic) {
      diagnostic->code = PipelineErrorCode::kDuplicateModelId;
      diagnostic->path = "/models";
      diagnostic->message =
          "Failed to atomically register batch models in ModelManager";
    }
    return false;
  }

  // 2. 解析 execution_mode 执行策略与线程池
  if (parsed_cfg.execution_mode == "parallel") {
    execution_mode_ = ExecutionMode::PARALLEL;
    max_parallel_workers_ = parsed_cfg.max_parallel_workers;
    thread_pool_ = std::make_unique<ThreadPool>(max_parallel_workers_);
    ALG_LOG_INFO(
        "[Pipeline] Parallel Wavefront Execution Mode enabled (workers: %zu)\n",
        max_parallel_workers_);
  } else {
    execution_mode_ = ExecutionMode::SEQUENTIAL;
    thread_pool_.reset();
    ALG_LOG_INFO("[Pipeline] Sequential Execution Mode active\n");
  }

  // 3. 按照波前拓扑层直接物化算子节点
  std::unordered_map<std::string, const ParsedNodeConfig*> node_by_id;
  for (const auto& node_cfg : parsed_cfg.nodes) {
    node_by_id[node_cfg.id] = &node_cfg;
  }

  for (size_t layer_idx = 0; layer_idx < plan_.topological_layers.size();
       ++layer_idx) {
    std::vector<INode*> layer_ptrs;
    for (const auto& node_id : plan_.topological_layers[layer_idx]) {
      auto it = node_by_id.find(node_id);
      if (it == node_by_id.end() || !it->second) continue;
      const auto& meta = *it->second;

      std::unique_ptr<INode> node;
      try {
        node = NodeFactory::Instance().Create(meta.node_type);
      } catch (const std::exception& e) {
        if (diagnostic) {
          diagnostic->code = PipelineErrorCode::kNodeCreateFailed;
          diagnostic->path =
              "/pipeline/" + std::to_string(meta.source_index) + "/node_type";
          diagnostic->message =
              "Exception creating node '" + meta.node_type + "': " + e.what();
        }
        return false;
      } catch (...) {
        if (diagnostic) {
          diagnostic->code = PipelineErrorCode::kNodeCreateFailed;
          diagnostic->path =
              "/pipeline/" + std::to_string(meta.source_index) + "/node_type";
          diagnostic->message =
              "Unknown exception creating node '" + meta.node_type + "'";
        }
        return false;
      }

      if (!node) {
        if (diagnostic) {
          diagnostic->code = PipelineErrorCode::kNodeCreateFailed;
          diagnostic->path =
              "/pipeline/" + std::to_string(meta.source_index) + "/node_type";
          diagnostic->message =
              "NodeFactory returned null for node_type: " + meta.node_type;
        }
        ALG_LOG_ERROR("[Pipeline] Failed to create node: %s\n",
                      meta.node_type.c_str());
        return false;
      }

      bool init_ok = false;
      try {
        const ValidatedNodePlan* node_plan_ptr = nullptr;
        auto np_it = plan_.node_plans.find(meta.id);
        if (np_it != plan_.node_plans.end()) {
          node_plan_ptr = &np_it->second;
        }

        NodeInitContext init_ctx;
        init_ctx.plan = node_plan_ptr;
        init_ctx.config =
            node_plan_ptr ? &node_plan_ptr->normalized_config : &meta.config;
        init_ctx.session_ctx = &session_ctx_;

        init_ok = node->Init(init_ctx);
      } catch (const std::exception& e) {
        if (diagnostic) {
          diagnostic->code = PipelineErrorCode::kNodeInitFailed;
          diagnostic->path =
              "/pipeline/" + std::to_string(meta.source_index) + "/config";
          diagnostic->message = "Exception initializing node '" +
                                meta.node_type + "': " + e.what();
        }
        return false;
      } catch (...) {
        if (diagnostic) {
          diagnostic->code = PipelineErrorCode::kNodeInitFailed;
          diagnostic->path =
              "/pipeline/" + std::to_string(meta.source_index) + "/config";
          diagnostic->message =
              "Unknown exception initializing node '" + meta.node_type + "'";
        }
        return false;
      }

      if (!init_ok) {
        if (diagnostic) {
          diagnostic->code = PipelineErrorCode::kNodeInitFailed;
          diagnostic->path =
              "/pipeline/" + std::to_string(meta.source_index) + "/config";
          diagnostic->message = "Failed to initialize node '" + meta.node_type +
                                "' (id: " + meta.id + ")";
        }
        ALG_LOG_ERROR("[Pipeline] Failed to initialize node: %s (id: %s)\n",
                      meta.node_type.c_str(), meta.id.c_str());
        return false;
      }

      layer_ptrs.push_back(node.get());
      nodes_.push_back(std::move(node));
      ALG_LOG_DEBUG("[Pipeline] Initialized node [%s] (id: %s, layer: %zu)\n",
                    meta.node_type.c_str(), meta.id.c_str(), layer_idx);
    }
    node_layers_.push_back(std::move(layer_ptrs));
  }

  ALG_LOG_DEBUG(
      "[Pipeline] DAG Wavefront Topology created with %zu execution layers:\n",
      node_layers_.size());
  for (size_t i = 0; i < plan_.topological_layers.size(); ++i) {
    std::string node_ids;
    for (size_t j = 0; j < plan_.topological_layers[i].size(); ++j) {
      node_ids += plan_.topological_layers[i][j];
      if (j + 1 < plan_.topological_layers[i].size()) node_ids += ", ";
    }
    ALG_LOG_DEBUG("  Layer %zu [%s]: %s\n", i,
                  node_layers_[i].size() > 1 ? "Parallel" : "Sequential",
                  node_ids.c_str());
  }

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
      // 多节点并发层：分发到线程池并发执行，并等待本波前汇聚
      std::vector<std::future<int>> futures;
      futures.reserve(layer.size());

      for (auto* node : layer) {
        futures.push_back(thread_pool_->Submit(
            [node, req_ctx]() { return node->Process(req_ctx); }));
      }

      int first_error = 0;
      for (size_t i = 0; i < futures.size(); ++i) {
        int ret = futures[i].get();
        if (ret != 0 && first_error == 0) {
          first_error = ret;
          ALG_LOG_ERROR(
              "[Pipeline] Parallel Node [%s] failed with error code: %d, "
              "msg: %s\n",
              layer[i]->Name().c_str(), ret,
              req_ctx->GetErrorMessage().c_str());
        }
      }

      if (first_error != 0) {
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

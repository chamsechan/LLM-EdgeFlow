#include "core/pipeline.h"

#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "core/node_registry.h"
#include "core/pipeline_validator.h"
#include "engine/engine_registry.h"

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
    case DiagnosticCode::kSerializedEngineConcurrency:
      return PipelineErrorCode::kInvalidCombination;
    case DiagnosticCode::kDuplicateModelId:
      return PipelineErrorCode::kDuplicateModelId;
    case DiagnosticCode::kDuplicateNodeId:
      return PipelineErrorCode::kDuplicateNodeId;
    case DiagnosticCode::kUnknownNodeType:
      return PipelineErrorCode::kUnknownNodeType;
    case DiagnosticCode::kUnknownEngineType:
      return PipelineErrorCode::kUnknownEngineType;
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

Pipeline::Pipeline()
    : biz_name_("default_biz"), business_name_("default_biz") {}

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
    std::cerr << "[Pipeline] Build attempted on non-empty Pipeline (state: "
              << static_cast<int>(state_) << ")" << std::endl;
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
    std::cerr << "[Pipeline] Failed to open config file: " << config_file_path
              << std::endl;
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
    std::cerr << "[Pipeline] JSON parse exception in " << config_file_path
              << ": " << e.what() << std::endl;
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
    std::cerr << "[Pipeline] Build attempted on non-empty Pipeline (state: "
              << static_cast<int>(state_) << ")" << std::endl;
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
    std::cerr
        << "[Pipeline] Unhandled internal exception during pipeline build: "
        << e.what() << std::endl;
  } catch (...) {
    success = false;
    if (diagnostic) {
      diagnostic->code = PipelineErrorCode::kInternalException;
      diagnostic->path = "/";
      diagnostic->message = "Unknown internal exception during pipeline build";
    }
    std::cerr << "[Pipeline] Unknown internal exception during pipeline build"
              << std::endl;
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
  plan_ = PipelineValidator::ValidateAndPlan(root_config, policy);

  if (!plan_.report.ok) {
    if (!plan_.report.diagnostics.empty()) {
      const auto& item = plan_.report.diagnostics.front();
      const char* code_str = DiagnosticCodeName(item.code);
      if (diagnostic) {
        diagnostic->code = ValidationCodeToPipelineCode(item.code);
        diagnostic->path = item.path;
        diagnostic->message = std::string(code_str) + ": " + item.message;
      }
      std::cerr << "[Pipeline] Validation failed: " << code_str << " at "
                << item.path << ": " << item.message << std::endl;
    }
    return false;
  }

  const auto& parsed_cfg = plan_.config;
  biz_name_ = parsed_cfg.biz_name;
  business_name_ = parsed_cfg.biz_name;

  // 1. 加载配置中声明的所有模型 (支持多模型加载到 ModelManager)
  const auto& options = session_ctx_.GetRuntimeOptions();
  for (const auto& model_cfg : parsed_cfg.models) {
    std::unique_ptr<IModelEngine> engine;
    try {
      engine = EngineFactory::Instance().Create(model_cfg.engine_type);
    } catch (const std::exception& e) {
      if (diagnostic) {
        diagnostic->code = PipelineErrorCode::kEngineCreateFailed;
        diagnostic->path = "/models/" + std::to_string(model_cfg.source_index) +
                           "/engine_type";
        diagnostic->message = "Exception creating engine '" +
                              model_cfg.engine_type + "': " + e.what();
      }
      return false;
    } catch (...) {
      if (diagnostic) {
        diagnostic->code = PipelineErrorCode::kEngineCreateFailed;
        diagnostic->path = "/models/" + std::to_string(model_cfg.source_index) +
                           "/engine_type";
        diagnostic->message =
            "Unknown exception creating engine '" + model_cfg.engine_type + "'";
      }
      return false;
    }

    if (!engine) {
      if (diagnostic) {
        diagnostic->code = PipelineErrorCode::kEngineCreateFailed;
        diagnostic->path = "/models/" + std::to_string(model_cfg.source_index) +
                           "/engine_type";
        diagnostic->message = "EngineFactory returned null for engine_type: " +
                              model_cfg.engine_type;
      }
      return false;
    }

    // 根据 RuntimeOptions 自动解析相对模型路径并注入 device_id
    std::string resolved_model_path = model_cfg.model_path;
    if (!model_cfg.model_path.empty()) {
      std::filesystem::path model_p(model_cfg.model_path);
      if (model_p.is_absolute()) {
        resolved_model_path = model_p.lexically_normal().string();
      } else if (!options.model_root_dir.empty()) {
        std::filesystem::path root_p(options.model_root_dir);

        std::string stripped_rel = model_cfg.model_path;
        if (stripped_rel.rfind("./models/", 0) == 0) {
          stripped_rel = stripped_rel.substr(9);
        } else if (stripped_rel.rfind("models/", 0) == 0) {
          stripped_rel = stripped_rel.substr(7);
        }

        std::filesystem::path cand_stripped = root_p / stripped_rel;
        std::filesystem::path cand_direct = root_p / model_p;
        std::filesystem::path cand_filename = root_p / model_p.filename();

        if (std::filesystem::exists(cand_stripped)) {
          resolved_model_path = cand_stripped.lexically_normal().string();
        } else if (std::filesystem::exists(cand_direct)) {
          resolved_model_path = cand_direct.lexically_normal().string();
        } else if (std::filesystem::exists(cand_filename)) {
          resolved_model_path = cand_filename.lexically_normal().string();
        } else if (std::filesystem::exists(model_p)) {
          resolved_model_path = model_p.lexically_normal().string();
        } else {
          resolved_model_path = cand_stripped.lexically_normal().string();
        }
      }
    }

    nlohmann::json custom_cfg = model_cfg.config;
    if (options.has_device_id && (!custom_cfg.contains("device_id") ||
                                  custom_cfg["device_id"].is_null())) {
      custom_cfg["device_id"] = options.device_id;
    }

    bool load_ok = false;
    try {
      load_ok = engine->Load(resolved_model_path, custom_cfg);
    } catch (const std::exception& e) {
      if (diagnostic) {
        diagnostic->code = PipelineErrorCode::kEngineLoadFailed;
        diagnostic->path = "/models/" + std::to_string(model_cfg.source_index);
        diagnostic->message =
            "Exception loading model '" + model_cfg.model_id + "': " + e.what();
      }
      return false;
    } catch (...) {
      if (diagnostic) {
        diagnostic->code = PipelineErrorCode::kEngineLoadFailed;
        diagnostic->path = "/models/" + std::to_string(model_cfg.source_index);
        diagnostic->message =
            "Unknown exception loading model '" + model_cfg.model_id + "'";
      }
      return false;
    }

    if (!load_ok) {
      if (diagnostic) {
        diagnostic->code = PipelineErrorCode::kEngineLoadFailed;
        diagnostic->path = "/models/" + std::to_string(model_cfg.source_index);
        diagnostic->message = "Failed to load model: " + model_cfg.model_id +
                              " at path: " + resolved_model_path;
      }
      std::cerr << "[Pipeline] Failed to load model: " << model_cfg.model_id
                << " at path: " << resolved_model_path << std::endl;
      return false;
    }

    if (!session_ctx_.GetModelManager().RegisterModel(
            model_cfg.model_id,
            std::shared_ptr<IModelEngine>(std::move(engine)))) {
      if (diagnostic) {
        diagnostic->code = PipelineErrorCode::kDuplicateModelId;
        diagnostic->path =
            "/models/" + std::to_string(model_cfg.source_index) + "/model_id";
        diagnostic->message =
            "Failed to register model in ModelManager: " + model_cfg.model_id;
      }
      return false;
    }

    std::cout << "[Pipeline] Successfully loaded model [" << model_cfg.model_id
              << "] with engine [" << model_cfg.engine_type << "]" << std::endl;
  }

  // 2. 解析 execution_mode 执行策略与线程池
  if (parsed_cfg.execution_mode == "parallel") {
    execution_mode_ = ExecutionMode::PARALLEL;
    max_parallel_workers_ = parsed_cfg.max_parallel_workers;
    thread_pool_ = std::make_unique<ThreadPool>(max_parallel_workers_);
    std::cout << "[Pipeline] Parallel Wavefront Execution Mode enabled "
                 "(workers: "
              << max_parallel_workers_ << ")" << std::endl;
  } else {
    execution_mode_ = ExecutionMode::SEQUENTIAL;
    thread_pool_.reset();
    std::cout << "[Pipeline] Sequential Execution Mode active" << std::endl;
  }

  // 3. 按照波前拓扑层直接物化算子节点
  topological_order_ = plan_.topological_order;
  topological_layers_ids_ = plan_.topological_layers;

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
        std::cerr << "[Pipeline] Failed to create node: " << meta.node_type
                  << std::endl;
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
        init_ctx.config = &meta.config;
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
        std::cerr << "[Pipeline] Failed to initialize node: " << meta.node_type
                  << " (id: " << meta.id << ")" << std::endl;
        return false;
      }

      layer_ptrs.push_back(node.get());
      nodes_.push_back(std::move(node));
      std::cout << "[Pipeline] Initialized node [" << meta.node_type
                << "] (id: " << meta.id << ", layer: " << layer_idx << ")"
                << std::endl;
    }
    node_layers_.push_back(std::move(layer_ptrs));
  }

  std::cout << "[Pipeline] DAG Wavefront Topology created with "
            << node_layers_.size() << " execution layers:" << std::endl;
  for (size_t i = 0; i < topological_layers_ids_.size(); ++i) {
    std::cout << "  Layer " << i << " ["
              << (node_layers_[i].size() > 1 ? "Parallel" : "Sequential")
              << "]: ";
    for (size_t j = 0; j < topological_layers_ids_[i].size(); ++j) {
      std::cout << topological_layers_ids_[i][j]
                << (j + 1 < topological_layers_ids_[i].size() ? ", " : "");
    }
    std::cout << std::endl;
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
          std::cerr << "[Pipeline] Node [" << node->Name()
                    << "] failed with error code: " << ret
                    << ", msg: " << req_ctx->GetErrorMessage() << std::endl;
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
          std::cerr << "[Pipeline] Parallel Node [" << layer[i]->Name()
                    << "] failed with error code: " << ret
                    << ", msg: " << req_ctx->GetErrorMessage() << std::endl;
        }
      }

      if (first_error != 0) {
        return first_error;
      }
    }
  }

  return 0;
}

int Pipeline::Control(int cmd, const std::string& json_param) {
  // R1-ACC-002: 仅允许在 Ready 状态下执行
  if (state_ != State::kReady) {
    return -1;
  }

  std::cout << "[Pipeline] Control cmd received: " << cmd
            << ", params: " << json_param << std::endl;

  bool has_target = false;
  bool has_handled = false;
  int first_fail_code = 0;

  for (auto& node : nodes_) {
    const auto* def = PipelineCatalog::FindNode(node->Name());
    bool supports_cmd = false;
    if (def) {
      for (const auto& cmd_def : def->control_commands) {
        if (cmd_def.cmd_id == cmd) {
          supports_cmd = true;
          break;
        }
      }
    }
    if (!supports_cmd) {
      continue;
    }

    has_target = true;
    NodeControlResult res = node->Control(cmd, json_param);
    if (res.status == NodeControlStatus::kFailed) {
      std::cerr << "[Pipeline] Node [" << node->Name()
                << "] Control failed with code: " << res.code
                << ", msg: " << res.message << std::endl;
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
    std::cerr << "[Pipeline] Unsupported control command: " << cmd << std::endl;
    return -7;  // COMPANY_ALG_ERR_UNSUPPORTED_CONTROL
  }
  return -7;
}

}  // namespace alg_framework

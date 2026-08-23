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

PipelineErrorCode ValidationCodeToPipelineCode(const std::string& code) {
  if (code == "ROOT_TYPE") return PipelineErrorCode::kRootType;
  if (code == "UNKNOWN_FIELD" || code == "UNKNOWN_CONFIG_FIELD")
    return PipelineErrorCode::kUnknownField;
  if (code == "MISSING_FIELD") return PipelineErrorCode::kMissingField;
  if (code == "FIELD_TYPE" || code == "CONFIG_FIELD_TYPE")
    return PipelineErrorCode::kFieldType;
  if (code == "FIELD_RANGE" || code == "CONFIG_FIELD_RANGE")
    return PipelineErrorCode::kFieldRange;
  if (code == "DUPLICATE_MODEL_ID") return PipelineErrorCode::kDuplicateModelId;
  if (code == "DUPLICATE_NODE_ID") return PipelineErrorCode::kDuplicateNodeId;
  if (code == "UNKNOWN_NODE_TYPE") return PipelineErrorCode::kUnknownNodeType;
  if (code == "UNKNOWN_ENGINE_TYPE")
    return PipelineErrorCode::kUnknownEngineType;
  if (code == "INVALID_DEPENDENCY" || code == "DUPLICATE_DEPENDENCY")
    return PipelineErrorCode::kInvalidDependency;
  if (code == "DAG_CYCLE") return PipelineErrorCode::kDagCycle;
  if (code == "REGISTRY_CONFLICT") return PipelineErrorCode::kRegistryConflict;
  return PipelineErrorCode::kInvalidCombination;
}

// Unit tests and downstream embedders may register private node/engine types
// without publishing a studio definition. The public validator remains strict;
// Pipeline only exempts these definition-only diagnostics for registered
// extension types so the historical extension API remains source-compatible.
bool IsPrivateExtensionDiagnostic(const ValidationDiagnostic& diagnostic) {
  if (diagnostic.code == "UNKNOWN_BUSINESS") return true;
  if (diagnostic.code == "UNKNOWN_NODE_TYPE" && !diagnostic.node_id.empty()) {
    return diagnostic.message.find("missing catalog definition") !=
           std::string::npos;
  }
  if (diagnostic.code == "UNKNOWN_ENGINE_TYPE") {
    constexpr const char* prefix = "Unknown engine_type: ";
    if (diagnostic.message.rfind(prefix, 0) == 0) {
      return EngineFactory::Instance().Has(diagnostic.message.substr(21));
    }
  }
  return false;
}

}  // namespace

Pipeline::Pipeline() : business_name_("default_biz") {}

bool Pipeline::BuildFromConfigFile(const std::string& config_file_path,
                                   PipelineDiagnostic* diagnostic) {
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

  return BuildFromJson(root_json, diagnostic);
}

bool Pipeline::BuildFromJson(const nlohmann::json& root_config,
                             PipelineDiagnostic* diagnostic) {
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
    success = BuildInternal(root_config, diagnostic);
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

bool Pipeline::ResolveDagTopologicalSort(
    const std::vector<ParsedNodeConfig>& raw_nodes, bool uses_explicit_dag,
    DagPlan* plan, PipelineDiagnostic* diagnostic) {
  if (!plan) return false;
  plan->topological_order.clear();
  plan->topological_layers_ids.clear();
  plan->sorted_layers.clear();

  if (raw_nodes.empty()) {
    return true;
  }

  // 1. 向后兼容处理：非显式 DAG (sequential 且全量节点均未声明 depends_on)
  // 直接保持 JSON 数组自然顺序
  if (!uses_explicit_dag) {
    for (const auto& node : raw_nodes) {
      plan->sorted_layers.push_back({node});
      plan->topological_order.push_back(node.id);
      plan->topological_layers_ids.push_back({node.id});
    }
    return true;
  }

  // 2. 建立节点 ID 查找索引
  std::unordered_map<std::string, const ParsedNodeConfig*> meta_lookup;
  for (const auto& node : raw_nodes) {
    meta_lookup[node.id] = &node;
  }

  // 3. 校验所有依赖 ID 是否均存在于节点集合中，并拦截自环死锁
  for (const auto& node : raw_nodes) {
    for (size_t d = 0; d < node.depends_on.size(); ++d) {
      const auto& dep_id = node.depends_on[d];
      std::string dep_path = "/pipeline/" + std::to_string(node.source_index) +
                             "/depends_on/" + std::to_string(d);
      if (dep_id == node.id) {
        if (diagnostic) {
          diagnostic->code = PipelineErrorCode::kDagCycle;
          diagnostic->path = dep_path;
          diagnostic->message = "Self-loop cycle detected: node '" + node.id +
                                "' depends on itself";
        }
        std::cerr << "[Pipeline] Self-loop cycle detected: node [" << node.id
                  << "] depends on itself!" << std::endl;
        return false;
      }
      if (meta_lookup.find(dep_id) == meta_lookup.end()) {
        if (diagnostic) {
          diagnostic->code = PipelineErrorCode::kInvalidDependency;
          diagnostic->path = dep_path;
          diagnostic->message = "Node '" + node.id +
                                "' depends on non-existent node '" + dep_id +
                                "'";
        }
        std::cerr << "[Pipeline] Invalid dependency: node [" << node.id
                  << "] depends on non-existent node [" << dep_id << "]"
                  << std::endl;
        return false;
      }
    }
  }

  // 4. 构建入度表 (In-Degree Map) 与邻接表 (Adjacency List: u -> v)
  std::unordered_map<std::string, int> in_degree;
  std::unordered_map<std::string, std::vector<std::string>> adj_list;

  for (const auto& node : raw_nodes) {
    in_degree[node.id] = static_cast<int>(node.depends_on.size());
    for (const auto& dep_id : node.depends_on) {
      adj_list[dep_id].push_back(node.id);
    }
  }

  // 5. Kahn 算法波前分层解析 (Wavefront BFS)
  std::queue<std::string> current_wave_q;
  for (const auto& node : raw_nodes) {
    if (in_degree[node.id] == 0) {
      current_wave_q.push(node.id);
    }
  }

  size_t total_resolved = 0;

  while (!current_wave_q.empty()) {
    size_t wave_size = current_wave_q.size();
    std::vector<ParsedNodeConfig> current_layer_nodes;
    std::vector<std::string> current_layer_ids;
    std::queue<std::string> next_wave_q;

    for (size_t i = 0; i < wave_size; ++i) {
      std::string u = current_wave_q.front();
      current_wave_q.pop();
      total_resolved++;

      current_layer_nodes.push_back(*meta_lookup[u]);
      current_layer_ids.push_back(u);
      plan->topological_order.push_back(u);

      auto it = adj_list.find(u);
      if (it != adj_list.end()) {
        for (const auto& v : it->second) {
          if (--in_degree[v] == 0) {
            next_wave_q.push(v);
          }
        }
      }
    }

    plan->sorted_layers.push_back(std::move(current_layer_nodes));
    plan->topological_layers_ids.push_back(std::move(current_layer_ids));
    current_wave_q = std::move(next_wave_q);
  }

  // 6. 环路死锁检测 (Cycle Deadlock Detection)
  if (total_resolved != raw_nodes.size()) {
    std::string unresolved_path = "/pipeline";
    std::string unresolved_name;
    for (const auto& node : raw_nodes) {
      if (in_degree[node.id] > 0) {
        unresolved_path = "/pipeline/" + std::to_string(node.source_index);
        unresolved_name = node.id;
        break;
      }
    }
    if (diagnostic) {
      diagnostic->code = PipelineErrorCode::kDagCycle;
      diagnostic->path = unresolved_path;
      diagnostic->message =
          "Cyclic dependency deadlock detected in DAG pipeline involving node "
          "'" +
          unresolved_name + "'";
    }
    std::cerr << "[Pipeline] Cyclic dependency deadlock detected in DAG "
                 "pipeline! Unresolved nodes: ";
    for (const auto& pair : in_degree) {
      if (pair.second > 0) {
        std::cerr << "[" << pair.first
                  << " (remaining in-degree: " << pair.second << ")] ";
      }
    }
    std::cerr << std::endl;
    return false;
  }

  return true;
}

bool Pipeline::BuildInternal(const nlohmann::json& root_config,
                             PipelineDiagnostic* diagnostic) {
  // FINAL-R1-003: 仅在测试场景下注入异常，以提供 kInternalException
  // 动态覆盖证据
  if (test_internal_hook_) {
    test_internal_hook_();
  }

  // Studio, CLI and runtime share this side-effect-free preflight. Engines and
  // nodes are materialized only after validation succeeds.
  const ValidationReport validation = PipelineValidator::Validate(root_config);
  for (const auto& item : validation.diagnostics) {
    if (IsPrivateExtensionDiagnostic(item)) continue;
    if (diagnostic) {
      diagnostic->code = ValidationCodeToPipelineCode(item.code);
      diagnostic->path = item.path;
      diagnostic->message = item.code + ": " + item.message;
    }
    std::cerr << "[Pipeline] Validation failed: " << item.code << " at "
              << item.path << ": " << item.message << std::endl;
    return false;
  }

  // =========================================================================
  // Phase 1: Parse
  // =========================================================================
  ParsedPipelineConfig parsed_cfg;
  if (!ParsePipelineConfig(root_config, &parsed_cfg, diagnostic)) {
    return false;
  }

  // =========================================================================
  // Phase 2: Validate References, Registry and DAG (Preflight before side
  // effects)
  // =========================================================================
  // 1. 校验 Registry 冲突状态 (fail-closed)
  if (NodeFactory::Instance().HasConflict()) {
    if (diagnostic) {
      diagnostic->code = PipelineErrorCode::kRegistryConflict;
      diagnostic->path = "/pipeline";
      diagnostic->message = "Node factory has registration conflict";
    }
    std::cerr << "[Pipeline] NodeFactory registration conflict detected!"
              << std::endl;
    return false;
  }

  if (EngineFactory::Instance().HasConflict()) {
    if (diagnostic) {
      diagnostic->code = PipelineErrorCode::kRegistryConflict;
      diagnostic->path = "/models";
      diagnostic->message = "Engine factory has registration conflict";
    }
    std::cerr << "[Pipeline] EngineFactory registration conflict detected!"
              << std::endl;
    return false;
  }

  // 2. 校验配置中声明的所有 engine_type 是否已注册
  for (const auto& model_cfg : parsed_cfg.models) {
    if (!EngineFactory::Instance().Has(model_cfg.engine_type)) {
      if (diagnostic) {
        diagnostic->code = PipelineErrorCode::kUnknownEngineType;
        diagnostic->path = "/models/" + std::to_string(model_cfg.source_index) +
                           "/engine_type";
        diagnostic->message = "Unknown engine_type: " + model_cfg.engine_type +
                              " for model: " + model_cfg.model_id;
      }
      std::cerr << "[Pipeline] Unknown engine_type: " << model_cfg.engine_type
                << " for model: " << model_cfg.model_id << std::endl;
      return false;
    }
  }

  // 3. 校验配置中声明的所有 node_type 是否已注册
  for (const auto& node_cfg : parsed_cfg.nodes) {
    if (!NodeFactory::Instance().Has(node_cfg.node_type)) {
      if (diagnostic) {
        diagnostic->code = PipelineErrorCode::kUnknownNodeType;
        diagnostic->path =
            "/pipeline/" + std::to_string(node_cfg.source_index) + "/node_type";
        diagnostic->message = "Unregistered node_type: " + node_cfg.node_type;
      }
      std::cerr << "[Pipeline] Unregistered node_type: " << node_cfg.node_type
                << " (id: " << node_cfg.id << ")" << std::endl;
      return false;
    }
  }

  // 4. 函数式解析 DAG 拓扑排序计划 (纯计算无副作用)
  DagPlan dag_plan;
  if (!ResolveDagTopologicalSort(parsed_cfg.nodes, parsed_cfg.uses_explicit_dag,
                                 &dag_plan, diagnostic)) {
    return false;
  }

  // =========================================================================
  // Phase 3: Materialize Engines and Nodes (with fine-grained exception
  // conversion)
  // =========================================================================
  business_name_ = parsed_cfg.business_name;

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

  // 3. 按照波前拓扑层实例化并初始化算子节点
  topological_order_ = std::move(dag_plan.topological_order);
  topological_layers_ids_ = std::move(dag_plan.topological_layers_ids);

  for (size_t layer_idx = 0; layer_idx < dag_plan.sorted_layers.size();
       ++layer_idx) {
    std::vector<INode*> layer_ptrs;
    for (const auto& meta : dag_plan.sorted_layers[layer_idx]) {
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
        init_ok = node->Init(meta.config, &session_ctx_);
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
          diagnostic->message =
              "Node Init returned false for node_type: " + meta.node_type +
              " (id: " + meta.id + ")";
        }
        std::cerr << "[Pipeline] Node Init failed: " << meta.node_type
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
  int last_ret = 0;
  for (auto& node : nodes_) {
    int ret = node->Control(cmd, json_param);
    if (ret != 0) {
      last_ret = ret;
    }
  }
  return last_ret;
}

}  // namespace alg_framework

#include "core/pipeline.h"

#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "core/node_registry.h"
#include "engine/engine_registry.h"

namespace alg_framework {

Pipeline::Pipeline() : business_name_("default_biz") {}

bool Pipeline::BuildFromConfigFile(const std::string& config_file_path) {
  std::ifstream ifs(config_file_path);
  if (!ifs.is_open()) {
    std::cerr << "[Pipeline] Failed to open config file: " << config_file_path
              << std::endl;
    return false;
  }

  try {
    nlohmann::json root_json;
    ifs >> root_json;
    return BuildFromJson(root_json);
  } catch (const std::exception& e) {
    std::cerr << "[Pipeline] JSON parse exception in " << config_file_path
              << ": " << e.what() << std::endl;
    return false;
  }
}

bool Pipeline::ResolveDagTopologicalSort(
    const std::vector<DagNodeMeta>& raw_nodes,
    std::vector<std::vector<DagNodeMeta>>* sorted_layers) {
  topological_order_.clear();
  topological_layers_ids_.clear();
  sorted_layers->clear();

  if (raw_nodes.empty()) {
    return true;
  }

  // 1. 提取所有节点 ID 并验证唯一性
  std::unordered_set<std::string> all_ids;
  std::unordered_map<std::string, const DagNodeMeta*> meta_lookup;
  bool has_explicit_dependencies = false;

  for (const auto& node : raw_nodes) {
    if (all_ids.find(node.id) != all_ids.end()) {
      std::cerr << "[Pipeline] Duplicate node id detected in DAG config: "
                << node.id << std::endl;
      return false;
    }
    all_ids.insert(node.id);
    meta_lookup[node.id] = &node;
    if (!node.depends_on.empty()) {
      has_explicit_dependencies = true;
    }
  }

  // 2. 向后兼容处理：若全量节点均未声明 depends_on，则直接保持 JSON
  // 数组自然顺序
  if (!has_explicit_dependencies) {
    for (const auto& node : raw_nodes) {
      sorted_layers->push_back({node});
      topological_order_.push_back(node.id);
      topological_layers_ids_.push_back({node.id});
    }
    return true;
  }

  // 3. 校验所有依赖 ID 是否均存在于节点集合中
  for (const auto& node : raw_nodes) {
    for (const auto& dep_id : node.depends_on) {
      if (dep_id == node.id) {
        std::cerr << "[Pipeline] Self-loop cycle detected: node [" << node.id
                  << "] depends on itself!" << std::endl;
        return false;
      }
      if (all_ids.find(dep_id) == all_ids.end()) {
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
    std::vector<DagNodeMeta> current_layer_nodes;
    std::vector<std::string> current_layer_ids;
    std::queue<std::string> next_wave_q;

    for (size_t i = 0; i < wave_size; ++i) {
      std::string u = current_wave_q.front();
      current_wave_q.pop();
      total_resolved++;

      current_layer_nodes.push_back(*meta_lookup[u]);
      current_layer_ids.push_back(u);
      topological_order_.push_back(u);

      auto it = adj_list.find(u);
      if (it != adj_list.end()) {
        for (const auto& v : it->second) {
          if (--in_degree[v] == 0) {
            next_wave_q.push(v);
          }
        }
      }
    }

    sorted_layers->push_back(std::move(current_layer_nodes));
    topological_layers_ids_.push_back(std::move(current_layer_ids));
    current_wave_q = std::move(next_wave_q);
  }

  // 6. 环路死锁检测 (Cycle Detection)
  if (total_resolved != raw_nodes.size()) {
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

bool Pipeline::BuildFromJson(const nlohmann::json& root_config) {
  business_name_ = root_config.value("business_name", "unnamed_biz");

  // 1. 加载配置中声明的所有模型 (支持多模型加载到 ModelManager)
  if (root_config.contains("models") && root_config["models"].is_array()) {
    for (const auto& model_cfg : root_config["models"]) {
      std::string model_id = model_cfg.value("model_id", "");
      std::string engine_type = model_cfg.value("engine_type", "");
      std::string model_path = model_cfg.value("model_path", "");
      nlohmann::json custom_cfg =
          model_cfg.value("config", nlohmann::json::object());

      if (model_id.empty() || engine_type.empty()) {
        std::cerr << "[Pipeline] Invalid model config: model_id or engine_type "
                     "is empty"
                  << std::endl;
        return false;
      }

      // 根据 RuntimeOptions 自动解析相对模型路径并注入 device_id
      std::string resolved_model_path = model_path;
      const auto& options = session_ctx_.GetRuntimeOptions();

      if (!model_path.empty()) {
        std::filesystem::path model_p(model_path);
        if (model_p.is_absolute()) {
          resolved_model_path = model_p.lexically_normal().string();
        } else if (!options.model_root_dir.empty()) {
          std::filesystem::path root_p(options.model_root_dir);

          std::string stripped_rel = model_path;
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

      // 只要显式指定了 device_id (has_device_id 为 true)，无论 0 还是非 0
      // 均注入 Engine
      if (options.has_device_id && (!custom_cfg.contains("device_id") ||
                                    custom_cfg["device_id"].is_null())) {
        custom_cfg["device_id"] = options.device_id;
      }

      auto engine = EngineFactory::Instance().Create(engine_type);
      if (!engine) {
        std::cerr << "[Pipeline] Unknown engine_type: " << engine_type
                  << " for model: " << model_id << std::endl;
        return false;
      }

      if (!engine->Load(resolved_model_path, custom_cfg)) {
        std::cerr << "[Pipeline] Failed to load model: " << model_id
                  << " at path: " << resolved_model_path << std::endl;
        return false;
      }

      session_ctx_.GetModelManager().RegisterModel(
          model_id, std::shared_ptr<IModelEngine>(std::move(engine)));
      std::cout << "[Pipeline] Successfully loaded model [" << model_id
                << "] with engine [" << engine_type << "]" << std::endl;
    }
  }

  // 2. 解析 execution_mode 执行策略与线程池
  std::string mode_str = root_config.value("execution_mode", "sequential");
  if (mode_str == "parallel" || mode_str == "async") {
    execution_mode_ = ExecutionMode::PARALLEL;
    max_parallel_workers_ = root_config.value("max_parallel_workers", 4);
    thread_pool_ = std::make_unique<ThreadPool>(max_parallel_workers_);
    std::cout << "[Pipeline] Parallel Wavefront Execution Mode enabled "
                 "(workers: "
              << max_parallel_workers_ << ")" << std::endl;
  } else {
    execution_mode_ = ExecutionMode::SEQUENTIAL;
    thread_pool_.reset();
    std::cout << "[Pipeline] Sequential Execution Mode active" << std::endl;
  }

  // 3. 解析 pipeline 数组并执行 DAG 拓扑分层波前排序
  if (!root_config.contains("pipeline") ||
      !root_config["pipeline"].is_array()) {
    std::cerr << "[Pipeline] Missing 'pipeline' array in configuration"
              << std::endl;
    return false;
  }

  std::vector<DagNodeMeta> raw_nodes;
  int auto_id_seq = 0;

  for (const auto& node_cfg : root_config["pipeline"]) {
    std::string node_type = node_cfg.value("node_type", "");
    if (node_type.empty()) {
      std::cerr << "[Pipeline] node_type is empty in pipeline config"
                << std::endl;
      return false;
    }

    std::string id = node_cfg.value("id", "");
    if (id.empty()) {
      id = "node_" + std::to_string(auto_id_seq) + "_" + node_type;
    }
    auto_id_seq++;

    std::vector<std::string> depends_on;
    if (node_cfg.contains("depends_on") && node_cfg["depends_on"].is_array()) {
      for (const auto& dep : node_cfg["depends_on"]) {
        if (dep.is_string() && !dep.get<std::string>().empty()) {
          depends_on.push_back(dep.get<std::string>());
        }
      }
    }

    nlohmann::json custom_cfg =
        node_cfg.value("config", nlohmann::json::object());

    // 合并顶层配置
    for (auto it = node_cfg.begin(); it != node_cfg.end(); ++it) {
      if (it.key() != "id" && it.key() != "node_type" &&
          it.key() != "depends_on" && it.key() != "config" &&
          it.key() != "comment") {
        custom_cfg[it.key()] = it.value();
      }
    }

    raw_nodes.push_back({id, node_type, std::move(depends_on), custom_cfg});
  }

  std::vector<std::vector<DagNodeMeta>> sorted_layers;
  if (!ResolveDagTopologicalSort(raw_nodes, &sorted_layers)) {
    std::cerr << "[Pipeline] DAG Topological Sort failed!" << std::endl;
    return false;
  }

  // 4. 按照波前拓扑层实例化并初始化算子节点
  nodes_.clear();
  node_layers_.clear();

  for (size_t layer_idx = 0; layer_idx < sorted_layers.size(); ++layer_idx) {
    std::vector<INode*> layer_ptrs;
    for (const auto& meta : sorted_layers[layer_idx]) {
      auto node = NodeFactory::Instance().Create(meta.node_type);
      if (!node) {
        std::cerr << "[Pipeline] Unregistered node_type: " << meta.node_type
                  << std::endl;
        return false;
      }

      if (!node->Init(meta.custom_config, &session_ctx_)) {
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
  if (!req_ctx) return -1;

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

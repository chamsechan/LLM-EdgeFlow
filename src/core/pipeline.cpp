#include "core/pipeline.h"

#include <fstream>
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
    std::vector<DagNodeMeta>* sorted_nodes) {
  topological_order_.clear();
  sorted_nodes->clear();

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
    *sorted_nodes = raw_nodes;
    for (const auto& node : raw_nodes) {
      topological_order_.push_back(node.id);
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

  // 4. 构建入度表 (In-Degree Map) 与邻接表 (Adjacency List: u -> v, 代表 u
  // 执行完成后可触发 v)
  std::unordered_map<std::string, int> in_degree;
  std::unordered_map<std::string, std::vector<std::string>> adj_list;

  for (const auto& node : raw_nodes) {
    in_degree[node.id] = static_cast<int>(node.depends_on.size());
    for (const auto& dep_id : node.depends_on) {
      adj_list[dep_id].push_back(node.id);
    }
  }

  // 5. 将所有入度为 0 的根节点入队 (Kahn 算法 BFS)
  std::queue<std::string> zero_in_degree_q;
  for (const auto& node : raw_nodes) {
    if (in_degree[node.id] == 0) {
      zero_in_degree_q.push(node.id);
    }
  }

  std::vector<std::string> sorted_ids;
  sorted_ids.reserve(raw_nodes.size());

  while (!zero_in_degree_q.empty()) {
    std::string u = zero_in_degree_q.front();
    zero_in_degree_q.pop();
    sorted_ids.push_back(u);

    auto it = adj_list.find(u);
    if (it != adj_list.end()) {
      for (const auto& v : it->second) {
        in_degree[v]--;
        if (in_degree[v] == 0) {
          zero_in_degree_q.push(v);
        }
      }
    }
  }

  // 6. 环路死锁检测 (Cycle Detection)
  if (sorted_ids.size() != raw_nodes.size()) {
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

  // 7. 按照拓扑序生成排好序的算子元数据列表
  sorted_nodes->reserve(sorted_ids.size());
  for (const auto& id : sorted_ids) {
    sorted_nodes->push_back(*meta_lookup[id]);
    topological_order_.push_back(id);
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

      auto engine = EngineFactory::Instance().Create(engine_type);
      if (!engine) {
        std::cerr << "[Pipeline] Unknown engine_type: " << engine_type
                  << " for model: " << model_id << std::endl;
        return false;
      }

      if (!engine->Load(model_path, custom_cfg)) {
        std::cerr << "[Pipeline] Failed to load model: " << model_id
                  << " at path: " << model_path << std::endl;
        return false;
      }

      session_ctx_.GetModelManager().RegisterModel(
          model_id, std::shared_ptr<IModelEngine>(std::move(engine)));
      std::cout << "[Pipeline] Successfully loaded model [" << model_id
                << "] with engine [" << engine_type << "]" << std::endl;
    }
  }

  // 2. 解析 pipeline 数组并执行 DAG 拓扑排序
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

    // 如果节点本身有其他顶层配置参数（如 model_id / threshold 等），合并入
    // custom_cfg
    for (auto it = node_cfg.begin(); it != node_cfg.end(); ++it) {
      if (it.key() != "id" && it.key() != "node_type" &&
          it.key() != "depends_on" && it.key() != "config" &&
          it.key() != "comment") {
        custom_cfg[it.key()] = it.value();
      }
    }

    raw_nodes.push_back({id, node_type, std::move(depends_on), custom_cfg});
  }

  std::vector<DagNodeMeta> sorted_nodes;
  if (!ResolveDagTopologicalSort(raw_nodes, &sorted_nodes)) {
    std::cerr << "[Pipeline] DAG Topological Sort failed!" << std::endl;
    return false;
  }

  // 3. 按照拓扑排序结果实例化并初始化算子节点
  nodes_.clear();
  for (const auto& meta : sorted_nodes) {
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

    nodes_.push_back(std::move(node));
    std::cout << "[Pipeline] Initialized node [" << meta.node_type
              << "] (id: " << meta.id << ")" << std::endl;
  }

  std::cout << "[Pipeline] DAG Topological Sort completed successfully. "
               "Execution order: ";
  for (size_t i = 0; i < topological_order_.size(); ++i) {
    std::cout << topological_order_[i]
              << (i + 1 < topological_order_.size() ? " -> " : "");
  }
  std::cout << std::endl;

  return true;
}

int Pipeline::Execute(AlgContext* req_ctx) {
  if (!req_ctx) return -1;

  for (size_t i = 0; i < nodes_.size(); ++i) {
    int ret = nodes_[i]->Process(req_ctx);
    if (ret != 0) {
      std::cerr << "[Pipeline] Node [" << nodes_[i]->Name()
                << "] failed with error code: " << ret
                << ", msg: " << req_ctx->GetErrorMessage() << std::endl;
      return ret;
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

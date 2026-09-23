#include "tools/pipeline_authoring.h"

#include <algorithm>
#include <stdexcept>

#include "core/pipeline_catalog.h"
#include "pipeline_document_validation.h"

namespace llm_edgeflow {

namespace {

nlohmann::json* FindNodeById(nlohmann::json* pipeline, const std::string& id) {
  if (!pipeline || !pipeline->contains("pipeline") ||
      !(*pipeline)["pipeline"].is_array()) {
    return nullptr;
  }
  nlohmann::json* found = nullptr;
  for (auto& node : (*pipeline)["pipeline"]) {
    if (node.is_object() && node.value("id", "") == id) {
      if (found) throw std::invalid_argument("AMBIGUOUS_NODE_ID: " + id);
      found = &node;
    }
  }
  return found;
}

std::string GetEffectiveOutputKey(const nlohmann::json& node,
                                  const std::string& port_name) {
  if (node.contains("outputs") && node["outputs"].is_object() &&
      node["outputs"].contains(port_name) &&
      node["outputs"][port_name].is_string()) {
    return node["outputs"][port_name].get<std::string>();
  }
  return port_name;
}

std::optional<std::string> GetEffectiveInputKey(const nlohmann::json& node,
                                                const std::string& port_name) {
  if (node.contains("inputs") && node["inputs"].is_object() &&
      node["inputs"].contains(port_name) &&
      node["inputs"][port_name].is_string()) {
    return node["inputs"][port_name].get<std::string>();
  }
  return std::nullopt;
}

// Validate the transient editing protocol, not the persisted Pipeline schema.
void CheckFields(const nlohmann::json& value,
                 std::initializer_list<const char*> allowed) {
  if (!value.is_object()) throw std::invalid_argument("字段必须是对象");
  for (const auto& item : value.items()) {
    if (std::none_of(allowed.begin(), allowed.end(),
                     [&](const char* key) { return item.key() == key; })) {
      throw std::invalid_argument("UNKNOWN_FIELD: " + item.key());
    }
  }
}

void RequireString(const nlohmann::json& value, const char* key) {
  if (!value.contains(key) || !value[key].is_string() ||
      value[key].get<std::string>().empty()) {
    throw std::invalid_argument(std::string("字段必须是非空字符串: ") + key);
  }
}

void CheckEndpoint(const nlohmann::json& endpoint) {
  CheckFields(endpoint, {"node_id", "port"});
  RequireString(endpoint, "node_id");
  RequireString(endpoint, "port");
}

void CheckOperation(const nlohmann::json& op) {
  RequireString(op, "kind");
  const auto kind = op["kind"].get<std::string>();
  if (kind == "add_node") {
    CheckFields(op, {"kind", "node_type", "id", "config"});
    RequireString(op, "node_type");
    if (op.contains("id")) RequireString(op, "id");
    if (op.contains("config") && !op["config"].is_object())
      throw std::invalid_argument("config 必须是对象");
  } else if (kind == "remove_node" || kind == "rename_node") {
    if (kind == "remove_node")
      CheckFields(op, {"kind", "node_id"});
    else {
      CheckFields(op, {"kind", "node_id", "new_id"});
      RequireString(op, "new_id");
    }
    RequireString(op, "node_id");
  } else if (kind == "connect" || kind == "disconnect") {
    CheckFields(op, {"kind", "source", "target"});
    CheckEndpoint(op.at("source"));
    CheckEndpoint(op.at("target"));
  } else if (kind == "add_dependency" || kind == "remove_dependency") {
    CheckFields(op, {"kind", "node_id", "depends_on_id"});
    RequireString(op, "node_id");
    RequireString(op, "depends_on_id");
  } else {
    throw std::invalid_argument("UNKNOWN_OPERATION_KIND: " + kind);
  }
}

std::string CheckGraphEndpoint(nlohmann::json* pipeline,
                               const nlohmann::json& endpoint, bool output) {
  const auto id = endpoint.at("node_id").get<std::string>();
  const auto port = endpoint.at("port").get<std::string>();
  if (id == "$ingress" || id == "$egress") {
    if ((output && id != "$ingress") || (!output && id != "$egress"))
      throw std::invalid_argument("INVALID_ENDPOINT: 业务端点方向错误");
    const auto biz = PipelineCatalog::FindBiz(pipeline->value("biz_name", ""));
    if (biz) {
      const auto& ports = output ? biz->ingress : biz->egress;
      for (const auto& definition : ports)
        if (definition.blackboard_key == port) return port;
    }
  } else {
    auto* node = FindNodeById(pipeline, id);
    if (!node) throw std::invalid_argument("NODE_NOT_FOUND: " + id);
    const auto definition =
        PipelineCatalog::FindNode(node->value("node_type", ""));
    if (definition) {
      const auto& ports = output ? definition->outputs : definition->inputs;
      for (const auto& candidate : ports) {
        if (candidate.logical_name == port)
          return output ? GetEffectiveOutputKey(*node, port) : port;
      }
    }
  }
  throw std::invalid_argument("UNKNOWN_PORT: " + id + "." + port);
}

void CheckUniqueProducer(const nlohmann::json& pipeline,
                         const std::string& key) {
  size_t count = 0;
  auto biz = PipelineCatalog::FindBiz(pipeline.value("biz_name", ""));
  if (biz)
    for (const auto& port : biz->ingress)
      if (port.blackboard_key == key) ++count;
  for (const auto& node : pipeline.at("pipeline")) {
    auto definition = PipelineCatalog::FindNode(node.value("node_type", ""));
    if (definition)
      for (const auto& port : definition->outputs)
        if (GetEffectiveOutputKey(node, port.logical_name) == key) ++count;
  }
  if (count != 1) throw std::invalid_argument("AMBIGUOUS_PRODUCER: " + key);
}

}  // namespace

nlohmann::json AuthoringChange::ToJson() const {
  nlohmann::json j = {{"action", action},
                      {"node_id", node_id},
                      {"affected_nodes", affected_nodes},
                      {"description", description}};
  if (!port.empty()) {
    j["port"] = port;
  }
  return j;
}

nlohmann::json AuthoringResult::ToJson() const {
  nlohmann::json j = {{"schema_version", schema_version}, {"ok", ok}};
  if (pipeline.has_value()) {
    j["pipeline"] = *pipeline;
  }
  j["changes"] = nlohmann::json::array();
  for (const auto& change : changes) {
    j["changes"].push_back(change.ToJson());
  }
  if (!validation.empty()) {
    j["validation"] = validation;
  }
  if (!diagnostics.empty()) {
    j["diagnostics"] = nlohmann::json::array();
    for (const auto& d : diagnostics) {
      j["diagnostics"].push_back({{"code", "AUTHORING_ERROR"},
                                  {"path", "/"},
                                  {"message", d},
                                  {"severity", "error"}});
    }
  }
  if (failed_operation_index.has_value()) {
    j["failed_operation_index"] = *failed_operation_index;
  }
  return j;
}

std::unordered_map<std::string, std::vector<std::string>>
PipelineAuthoring::BuildDependencyGraph(const nlohmann::json& pipeline) {
  std::unordered_map<std::string, std::vector<std::string>> graph;
  if (!pipeline.is_object() || !pipeline.contains("pipeline") ||
      !pipeline["pipeline"].is_array()) {
    return graph;
  }
  for (const auto& node : pipeline["pipeline"]) {
    if (!node.is_object() || !node.contains("id") || !node["id"].is_string()) {
      continue;
    }
    std::string id = node["id"].get<std::string>();
    std::vector<std::string> deps;
    if (node.contains("depends_on") && node["depends_on"].is_array()) {
      for (const auto& dep : node["depends_on"]) {
        if (dep.is_string()) {
          deps.push_back(dep.get<std::string>());
        }
      }
    }
    graph[id] = std::move(deps);
  }
  return graph;
}

bool PipelineAuthoring::IsAncestor(
    const std::string& potential_ancestor, const std::string& node_id,
    const std::unordered_map<std::string, std::vector<std::string>>&
        dep_graph) {
  if (potential_ancestor.empty() || node_id.empty()) return false;
  if (potential_ancestor == node_id) return true;
  std::unordered_set<std::string> visited;
  std::vector<std::string> stack;
  auto it = dep_graph.find(node_id);
  if (it != dep_graph.end()) {
    for (const auto& dep : it->second) {
      stack.push_back(dep);
    }
  }
  while (!stack.empty()) {
    std::string curr = stack.back();
    stack.pop_back();
    if (curr == potential_ancestor) return true;
    if (!visited.insert(curr).second) continue;
    auto curr_it = dep_graph.find(curr);
    if (curr_it != dep_graph.end()) {
      for (const auto& dep : curr_it->second) {
        if (visited.find(dep) == visited.end()) {
          stack.push_back(dep);
        }
      }
    }
  }
  return false;
}

std::unordered_set<std::string> PipelineAuthoring::GetOccupiedKeys(
    const nlohmann::json& pipeline) {
  std::unordered_set<std::string> occupied;
  if (!pipeline.is_object()) return occupied;

  std::string biz_name = pipeline.value("biz_name", "");
  if (!biz_name.empty()) {
    auto biz = PipelineCatalog::FindBiz(biz_name);
    if (biz) {
      for (const auto& in : biz->ingress) {
        occupied.insert(in.blackboard_key);
      }
      for (const auto& out : biz->egress) {
        occupied.insert(out.blackboard_key);
      }
    }
  }

  if (pipeline.contains("pipeline") && pipeline["pipeline"].is_array()) {
    for (const auto& node : pipeline["pipeline"]) {
      if (!node.is_object()) continue;
      std::string node_type = node.value("node_type", "");
      auto node_def = PipelineCatalog::FindNode(node_type);

      for (const auto* direction : {"inputs", "outputs"}) {
        if (node.contains(direction) && node[direction].is_object()) {
          for (const auto& value : node[direction]) {
            if (value.is_string()) occupied.insert(value.get<std::string>());
          }
        }
      }

      if (node_def) {
        for (const auto& out : node_def->outputs) {
          bool has_explicit = false;
          if (node.contains("outputs") && node["outputs"].is_object() &&
              node["outputs"].contains(out.logical_name)) {
            has_explicit = true;
          }
          if (!has_explicit) {
            occupied.insert(out.logical_name);
          }
        }
      }
    }
  }
  return occupied;
}

std::string PipelineAuthoring::AllocateKey(
    const std::string& base, const std::unordered_set<std::string>& occupied) {
  if (occupied.find(base) == occupied.end()) {
    return base;
  }
  int suffix = 1;
  while (true) {
    std::string cand = base + "_" + std::to_string(suffix);
    if (occupied.find(cand) == occupied.end()) {
      return cand;
    }
    ++suffix;
  }
}

bool PipelineAuthoring::ApplyOperation(nlohmann::json* pipeline,
                                       const nlohmann::json& operation,
                                       std::vector<AuthoringChange>* changes,
                                       std::string* error) {
  if (!pipeline || !pipeline->is_object() || !operation.is_object()) {
    if (error) *error = "无效的 pipeline 或 operation 对象";
    return false;
  }
  CheckOperation(operation);
  std::string kind = operation.value("kind", "");
  if (kind == "connect" || kind == "disconnect") {
    auto key = CheckGraphEndpoint(pipeline, operation.at("source"), true);
    CheckGraphEndpoint(pipeline, operation.at("target"), false);
    CheckUniqueProducer(*pipeline, key);
  }
  if (kind.empty()) {
    if (error) *error = "operation 缺少 kind 字段";
    return false;
  }

  std::string biz_name = pipeline->value("biz_name", "");
  auto biz = PipelineCatalog::FindBiz(biz_name);

  if (kind == "add_node") {
    std::string node_type = operation.value("node_type", "");
    if (node_type.empty()) {
      if (error) *error = "add_node 缺少 node_type 字段";
      return false;
    }
    auto node_def = PipelineCatalog::FindNode(node_type);
    if (!node_def) {
      if (error) *error = "UNKNOWN_NODE_TYPE: " + node_type;
      return false;
    }

    std::string node_id;
    if (operation.contains("id")) {
      if (!operation["id"].is_string()) {
        if (error) *error = "node id 必须是字符串";
        return false;
      }
      node_id = operation["id"].get<std::string>();
      if (node_id.empty()) {
        if (error) *error = "node id 不能为空";
        return false;
      }
      if (node_id == "$ingress" || node_id == "$egress") {
        if (error)
          *error = "RESERVED_NODE_ID: 节点 ID 不能使用保留名 " + node_id;
        return false;
      }
      if (FindNodeById(pipeline, node_id) != nullptr) {
        if (error) *error = "DUPLICATE_NODE_ID: " + node_id;
        return false;
      }
    } else {
      size_t index = (*pipeline)["pipeline"].size();
      while (true) {
        std::string cand = "node_" + std::to_string(index) + "_" + node_type;
        if (FindNodeById(pipeline, cand) == nullptr) {
          node_id = cand;
          break;
        }
        ++index;
      }
    }

    nlohmann::json config = operation.value("config", nlohmann::json::object());
    if (!config.is_object()) {
      if (error) *error = "config 必须是 JSON 对象";
      return false;
    }

    nlohmann::json new_node;
    new_node["id"] = node_id;
    new_node["node_type"] = node_type;
    new_node["inputs"] = nlohmann::json::object();
    new_node["outputs"] = nlohmann::json::object();
    new_node["config"] = config;

    auto occupied = GetOccupiedKeys(*pipeline);
    for (const auto& out : node_def->outputs) {
      std::string base = node_id + "__" + out.logical_name;
      std::string out_key = AllocateKey(base, occupied);
      occupied.insert(out_key);
      new_node["outputs"][out.logical_name] = out_key;
    }

    (*pipeline)["pipeline"].push_back(std::move(new_node));
    if (changes) {
      changes->push_back({"add_node",
                          node_id,
                          "",
                          {node_id},
                          "添加节点 " + node_id + " (" + node_type + ")"});
    }
    return true;
  }

  if (kind == "remove_node") {
    std::string node_id = operation.value("node_id", "");
    if (node_id.empty()) {
      if (error) *error = "remove_node 缺少 node_id";
      return false;
    }
    auto* target_node = FindNodeById(pipeline, node_id);
    if (!target_node) {
      if (error) *error = "NODE_NOT_FOUND: " + node_id;
      return false;
    }

    std::unordered_set<std::string> out_keys;
    if ((*target_node).contains("outputs") &&
        (*target_node)["outputs"].is_object()) {
      for (const auto& [k, v] : (*target_node)["outputs"].items()) {
        if (v.is_string()) {
          out_keys.insert(v.get<std::string>());
        }
      }
    }
    auto node_def =
        PipelineCatalog::FindNode(target_node->value("node_type", ""));
    if (node_def) {
      for (const auto& out : node_def->outputs) {
        if (!(*target_node).contains("outputs") ||
            !(*target_node)["outputs"].contains(out.logical_name)) {
          out_keys.insert(out.logical_name);
        }
      }
    }

    bool connects_to_egress = false;
    if (biz) {
      for (const auto& out_k : out_keys) {
        for (const auto& eg : biz->egress) {
          if (eg.blackboard_key == out_k) {
            connects_to_egress = true;
            break;
          }
        }
        if (connects_to_egress) break;
      }
    }

    std::vector<std::string> affected = {node_id};
    for (auto& other : (*pipeline)["pipeline"]) {
      if (!other.is_object() || other.value("id", "") == node_id) continue;
      std::string other_id = other.value("id", "");
      bool other_affected = false;
      if (other.contains("inputs") && other["inputs"].is_object()) {
        auto& inputs = other["inputs"];
        for (auto it = inputs.begin(); it != inputs.end();) {
          if (it->is_string() && out_keys.count(it->get<std::string>())) {
            it = inputs.erase(it);
            other_affected = true;
          } else {
            ++it;
          }
        }
      }

      if (other.contains("depends_on") && other["depends_on"].is_array()) {
        auto& deps = other["depends_on"];
        for (auto it = deps.begin(); it != deps.end();) {
          if (it->is_string() && it->get<std::string>() == node_id) {
            it = deps.erase(it);
            other_affected = true;
          } else {
            ++it;
          }
        }
      }

      if (other_affected) {
        affected.push_back(other_id);
      }
    }

    if (connects_to_egress) {
      affected.push_back("$egress");
    }

    auto& pipe_arr = (*pipeline)["pipeline"];
    for (auto it = pipe_arr.begin(); it != pipe_arr.end(); ++it) {
      if (it->is_object() && it->value("id", "") == node_id) {
        pipe_arr.erase(it);
        break;
      }
    }

    if (changes) {
      std::string desc = "删除节点 " + node_id;
      if (connects_to_egress) {
        desc += "（断开业务出口）";
      }
      changes->push_back({"remove_node", node_id, "", affected, desc});
    }
    return true;
  }

  if (kind == "rename_node") {
    std::string node_id = operation.value("node_id", "");
    if (!operation.contains("new_id") || !operation["new_id"].is_string()) {
      if (error) *error = "rename_node 缺少 new_id 字符串";
      return false;
    }
    std::string new_id = operation["new_id"].get<std::string>();
    if (node_id.empty() || new_id.empty()) {
      if (error) *error = "rename_node 缺少 node_id 或 new_id";
      return false;
    }
    if (new_id == "$ingress" || new_id == "$egress") {
      if (error) *error = "RESERVED_NODE_ID: 节点 ID 不能使用保留名 " + new_id;
      return false;
    }
    auto* target_node = FindNodeById(pipeline, node_id);
    if (!target_node) {
      if (error) *error = "NODE_NOT_FOUND: " + node_id;
      return false;
    }
    if (node_id == new_id) {
      if (changes) {
        changes->push_back({"rename_node",
                            node_id,
                            "",
                            {node_id},
                            "重命名节点 " + node_id + "（无变化）"});
      }
      return true;
    }
    if (FindNodeById(pipeline, new_id) != nullptr) {
      if (error) *error = "DUPLICATE_NODE_ID: " + new_id;
      return false;
    }

    (*target_node)["id"] = new_id;
    std::vector<std::string> affected = {new_id};
    for (auto& other : (*pipeline)["pipeline"]) {
      if (!other.is_object() || other.value("id", "") == new_id) continue;
      if (other.contains("depends_on") && other["depends_on"].is_array()) {
        bool updated = false;
        for (auto& dep : other["depends_on"]) {
          if (dep.is_string() && dep.get<std::string>() == node_id) {
            dep = new_id;
            updated = true;
          }
        }
        if (updated) {
          affected.push_back(other.value("id", ""));
        }
      }
    }

    if (changes) {
      changes->push_back({"rename_node", new_id, "", affected,
                          "重命名节点 " + node_id + " 为 " + new_id});
    }
    return true;
  }

  if (kind == "connect") {
    if (!operation.contains("source") || !operation["source"].is_object() ||
        !operation.contains("target") || !operation["target"].is_object()) {
      if (error) *error = "connect 缺少 source 或 target 对象";
      return false;
    }
    std::string src_id = operation["source"].value("node_id", "");
    std::string src_port = operation["source"].value("port", "");
    std::string tgt_id = operation["target"].value("node_id", "");
    std::string tgt_port = operation["target"].value("port", "");
    if (src_id.empty() || src_port.empty() || tgt_id.empty() ||
        tgt_port.empty()) {
      if (error) *error = "connect 端点 node_id 或 port 不能为空";
      return false;
    }

    if (src_id == "$ingress") {
      if (!biz) {
        if (error) *error = "UNKNOWN_BIZ: " + biz_name;
        return false;
      }
      auto in_it = std::find_if(biz->ingress.begin(), biz->ingress.end(),
                                [&](const BizPortDefinition& p) {
                                  return p.blackboard_key == src_port;
                                });
      if (in_it == biz->ingress.end()) {
        if (error) *error = "业务输入端口不存在: " + src_port;
        return false;
      }
      if (tgt_id == "$ingress" || tgt_id == "$egress") {
        if (error) *error = "目标节点不能是业务端口";
        return false;
      }
      auto* tgt_node = FindNodeById(pipeline, tgt_id);
      if (!tgt_node) {
        if (error) *error = "NODE_NOT_FOUND: " + tgt_id;
        return false;
      }
      auto tgt_def =
          PipelineCatalog::FindNode(tgt_node->value("node_type", ""));
      if (!tgt_def) {
        if (error)
          *error = "UNKNOWN_NODE_TYPE: " + tgt_node->value("node_type", "");
        return false;
      }
      auto tgt_p_it =
          std::find_if(tgt_def->inputs.begin(), tgt_def->inputs.end(),
                       [&](const NodePortDefinition& p) {
                         return p.logical_name == tgt_port;
                       });
      if (tgt_p_it == tgt_def->inputs.end()) {
        if (error) *error = "目标输入端口不存在: " + tgt_port;
        return false;
      }
      if (in_it->type_id != tgt_p_it->type_id) {
        if (error) {
          *error = "端口不存在或数据类型不兼容: " + in_it->type_id +
                   " != " + tgt_p_it->type_id;
        }
        return false;
      }
      if (!(*tgt_node).contains("inputs")) {
        (*tgt_node)["inputs"] = nlohmann::json::object();
      }
      (*tgt_node)["inputs"][tgt_port] = src_port;

      if (changes) {
        changes->push_back(
            {"connect",
             tgt_id,
             tgt_port,
             {tgt_id},
             "连接业务输入 " + src_port + " 到 " + tgt_id + "." + tgt_port});
      }
      return true;
    }

    if (tgt_id == "$egress") {
      if (!biz) {
        if (error) *error = "UNKNOWN_BIZ: " + biz_name;
        return false;
      }
      auto out_it = std::find_if(biz->egress.begin(), biz->egress.end(),
                                 [&](const BizPortDefinition& p) {
                                   return p.blackboard_key == tgt_port;
                                 });
      if (out_it == biz->egress.end()) {
        if (error) *error = "业务输出端口不存在: " + tgt_port;
        return false;
      }
      auto* src_node = FindNodeById(pipeline, src_id);
      if (!src_node) {
        if (error) *error = "NODE_NOT_FOUND: " + src_id;
        return false;
      }
      auto src_def =
          PipelineCatalog::FindNode(src_node->value("node_type", ""));
      if (!src_def) {
        if (error)
          *error = "UNKNOWN_NODE_TYPE: " + src_node->value("node_type", "");
        return false;
      }
      auto src_p_it =
          std::find_if(src_def->outputs.begin(), src_def->outputs.end(),
                       [&](const NodePortDefinition& p) {
                         return p.logical_name == src_port;
                       });
      if (src_p_it == src_def->outputs.end()) {
        if (error) *error = "源输出端口不存在: " + src_port;
        return false;
      }
      if (src_p_it->type_id != out_it->type_id) {
        if (error) {
          *error = "端口不存在或数据类型不兼容: " + src_p_it->type_id +
                   " != " + out_it->type_id;
        }
        return false;
      }

      // Check conflict: does any other node produce tgt_port?
      for (const auto& other : (*pipeline)["pipeline"]) {
        if (!other.is_object()) continue;
        std::string o_id = other.value("id", "");
        auto o_def = PipelineCatalog::FindNode(other.value("node_type", ""));
        if (o_id != src_id) {
          if (o_def) {
            for (const auto& out : o_def->outputs) {
              if (GetEffectiveOutputKey(other, out.logical_name) == tgt_port) {
                if (error) *error = "业务输出已有生产者，请先断开原连线";
                return false;
              }
            }
          }
        } else {
          if (o_def) {
            for (const auto& out : o_def->outputs) {
              if (out.logical_name != src_port &&
                  GetEffectiveOutputKey(other, out.logical_name) == tgt_port) {
                if (error) *error = "业务输出已有生产者，请先断开原连线";
                return false;
              }
            }
          }
        }
      }

      std::string current_key = src_port;
      if ((*src_node).contains("outputs") &&
          (*src_node)["outputs"].contains(src_port) &&
          (*src_node)["outputs"][src_port].is_string()) {
        current_key = (*src_node)["outputs"][src_port].get<std::string>();
      }

      // Check if current_key is already mapped to another egress port
      for (const auto& eg : biz->egress) {
        if (eg.blackboard_key != tgt_port && eg.blackboard_key == current_key) {
          if (error) {
            *error = "当前输出已连接到另一业务出口，无法发布到两个不同契约 key";
          }
          return false;
        }
      }

      if (current_key == tgt_port) {
        return true;
      }
      if (!(*src_node).contains("outputs")) {
        (*src_node)["outputs"] = nlohmann::json::object();
      }
      (*src_node)["outputs"][src_port] = tgt_port;

      std::vector<std::string> affected = {src_id};
      for (auto& consumer : (*pipeline)["pipeline"]) {
        if (!consumer.is_object()) continue;
        std::string c_id = consumer.value("id", "");
        if (c_id == src_id) continue;
        bool c_affected = false;
        if (consumer.contains("inputs") && consumer["inputs"].is_object()) {
          for (auto& [in_k, in_v] : consumer["inputs"].items()) {
            if (in_v.is_string() && in_v.get<std::string>() == current_key) {
              in_v = tgt_port;
              c_affected = true;
            }
          }
        }
        if (c_affected) affected.push_back(c_id);
      }

      if (changes) {
        changes->push_back(
            {"connect", src_id, src_port, affected,
             "连接 " + src_id + "." + src_port + " 到业务输出 " + tgt_port});
      }
      return true;
    }

    // Node to Node
    if (src_id == tgt_id) {
      if (error) *error = "连线会形成环";
      return false;
    }
    auto* src_node = FindNodeById(pipeline, src_id);
    auto* tgt_node = FindNodeById(pipeline, tgt_id);
    if (!src_node) {
      if (error) *error = "NODE_NOT_FOUND: " + src_id;
      return false;
    }
    if (!tgt_node) {
      if (error) *error = "NODE_NOT_FOUND: " + tgt_id;
      return false;
    }

    auto dep_graph = BuildDependencyGraph(*pipeline);
    if (IsAncestor(tgt_id, src_id, dep_graph)) {
      if (error) *error = "连线会形成环";
      return false;
    }

    auto src_def = PipelineCatalog::FindNode(src_node->value("node_type", ""));
    auto tgt_def = PipelineCatalog::FindNode(tgt_node->value("node_type", ""));
    if (!src_def) {
      if (error)
        *error = "UNKNOWN_NODE_TYPE: " + src_node->value("node_type", "");
      return false;
    }
    if (!tgt_def) {
      if (error)
        *error = "UNKNOWN_NODE_TYPE: " + tgt_node->value("node_type", "");
      return false;
    }

    auto src_p_it =
        std::find_if(src_def->outputs.begin(), src_def->outputs.end(),
                     [&](const NodePortDefinition& p) {
                       return p.logical_name == src_port;
                     });
    auto tgt_p_it = std::find_if(tgt_def->inputs.begin(), tgt_def->inputs.end(),
                                 [&](const NodePortDefinition& p) {
                                   return p.logical_name == tgt_port;
                                 });
    if (src_p_it == src_def->outputs.end()) {
      if (error) *error = "源输出端口不存在: " + src_port;
      return false;
    }
    if (tgt_p_it == tgt_def->inputs.end()) {
      if (error) *error = "目标输入端口不存在: " + tgt_port;
      return false;
    }
    if (src_p_it->type_id != tgt_p_it->type_id) {
      if (error) {
        *error = "端口不存在或数据类型不兼容: " + src_p_it->type_id +
                 " != " + tgt_p_it->type_id;
      }
      return false;
    }

    // Ordinary fan-out never renames the producer's existing effective key.
    std::string key = GetEffectiveOutputKey(*src_node, src_port);
    if (!(*tgt_node).contains("inputs")) {
      (*tgt_node)["inputs"] = nlohmann::json::object();
    }
    (*tgt_node)["inputs"][tgt_port] = key;

    if (changes) {
      changes->push_back({"connect",
                          tgt_id,
                          tgt_port,
                          {src_id, tgt_id},
                          "连接 " + src_id + "." + src_port + " 到 " + tgt_id +
                              "." + tgt_port});
    }
    return true;
  }

  if (kind == "disconnect") {
    if (!operation.contains("source") || !operation["source"].is_object() ||
        !operation.contains("target") || !operation["target"].is_object()) {
      if (error) *error = "disconnect 缺少 source 或 target 对象";
      return false;
    }
    std::string src_id = operation["source"].value("node_id", "");
    std::string src_port = operation["source"].value("port", "");
    std::string tgt_id = operation["target"].value("node_id", "");
    std::string tgt_port = operation["target"].value("port", "");

    if (tgt_id == "$egress") {
      auto* src_node = FindNodeById(pipeline, src_id);
      if (!src_node) {
        if (error) *error = "NODE_NOT_FOUND: " + src_id;
        return false;
      }
      std::string current_key = GetEffectiveOutputKey(*src_node, src_port);
      if (current_key != tgt_port) {
        if (error) *error = "连线不存在或已被修改";
        return false;
      }

      auto occupied = GetOccupiedKeys(*pipeline);
      std::string new_key = AllocateKey(src_id + "__" + src_port, occupied);
      occupied.insert(new_key);
      if (!(*src_node).contains("outputs") ||
          !(*src_node)["outputs"].is_object()) {
        (*src_node)["outputs"] = nlohmann::json::object();
      }
      (*src_node)["outputs"][src_port] = new_key;

      std::vector<std::string> affected = {src_id};
      for (auto& consumer : (*pipeline)["pipeline"]) {
        if (!consumer.is_object() || consumer.value("id", "") == src_id)
          continue;
        bool c_affected = false;
        if (consumer.contains("inputs") && consumer["inputs"].is_object()) {
          for (auto& [in_k, in_v] : consumer["inputs"].items()) {
            if (in_v.is_string() && in_v.get<std::string>() == tgt_port) {
              in_v = new_key;
              c_affected = true;
            }
          }
        }
        if (c_affected) affected.push_back(consumer.value("id", ""));
      }

      if (changes) {
        changes->push_back(
            {"disconnect", src_id, src_port, affected,
             "断开 " + src_id + "." + src_port + " 与业务输出 " + tgt_port});
      }
      return true;
    }

    if (src_id == "$ingress") {
      auto* tgt_node = FindNodeById(pipeline, tgt_id);
      if (!tgt_node) {
        if (error) *error = "NODE_NOT_FOUND: " + tgt_id;
        return false;
      }
      auto in_key_opt = GetEffectiveInputKey(*tgt_node, tgt_port);
      if (!in_key_opt.has_value() || *in_key_opt != src_port) {
        if (error) *error = "连线不存在或已被修改";
        return false;
      }

      (*tgt_node)["inputs"].erase(tgt_port);

      if (changes) {
        changes->push_back(
            {"disconnect",
             tgt_id,
             tgt_port,
             {tgt_id},
             "断开业务输入 " + src_port + " 到 " + tgt_id + "." + tgt_port});
      }
      return true;
    }

    // Normal node to Normal node
    auto* src_node = FindNodeById(pipeline, src_id);
    auto* tgt_node = FindNodeById(pipeline, tgt_id);
    if (!src_node) {
      if (error) *error = "NODE_NOT_FOUND: " + src_id;
      return false;
    }
    if (!tgt_node) {
      if (error) *error = "NODE_NOT_FOUND: " + tgt_id;
      return false;
    }

    std::string src_out_key = GetEffectiveOutputKey(*src_node, src_port);
    auto in_key_opt = GetEffectiveInputKey(*tgt_node, tgt_port);
    if (!in_key_opt.has_value() || *in_key_opt != src_out_key) {
      if (error) *error = "连线不存在或已被修改";
      return false;
    }

    (*tgt_node)["inputs"].erase(tgt_port);

    if (changes) {
      changes->push_back({"disconnect",
                          tgt_id,
                          tgt_port,
                          {src_id, tgt_id},
                          "断开 " + src_id + "." + src_port + " 到 " + tgt_id +
                              "." + tgt_port});
    }
    return true;
  }

  if (kind == "add_dependency") {
    std::string node_id = operation.value("node_id", "");
    std::string dep_id = operation.value("depends_on_id", "");
    if (node_id.empty() || dep_id.empty()) {
      if (error) *error = "add_dependency 缺少 node_id 或 depends_on_id";
      return false;
    }
    if (node_id == dep_id) {
      if (error) *error = "添加依赖会形成环";
      return false;
    }
    auto* node = FindNodeById(pipeline, node_id);
    auto* dep_node = FindNodeById(pipeline, dep_id);
    if (!node) {
      if (error) *error = "NODE_NOT_FOUND: " + node_id;
      return false;
    }
    if (!dep_node) {
      if (error) *error = "NODE_NOT_FOUND: " + dep_id;
      return false;
    }

    auto dep_graph = BuildDependencyGraph(*pipeline);
    if (IsAncestor(node_id, dep_id, dep_graph)) {
      if (error) *error = "添加依赖会形成环";
      return false;
    }

    if (IsAncestor(dep_id, node_id, dep_graph)) {
      // Already an ancestor; do not add redundant edge (RFC-0057 Section 4.5)
      if (changes) {
        changes->push_back({"add_dependency",
                            node_id,
                            "",
                            {node_id, dep_id},
                            "添加 " + node_id + " 对 " + dep_id +
                                " 的执行依赖（已存在祖先关系，无变化）"});
      }
      return true;
    }

    if (!node->contains("depends_on") || !(*node)["depends_on"].is_array()) {
      (*node)["depends_on"] = nlohmann::json::array();
    }
    bool already = false;
    for (const auto& d : (*node)["depends_on"]) {
      if (d.is_string() && d.get<std::string>() == dep_id) {
        already = true;
        break;
      }
    }
    if (!already) {
      (*node)["depends_on"].push_back(dep_id);
    }

    if (changes) {
      changes->push_back({"add_dependency",
                          node_id,
                          "",
                          {node_id, dep_id},
                          "添加 " + node_id + " 对 " + dep_id + " 的执行依赖"});
    }
    return true;
  }

  if (kind == "remove_dependency") {
    std::string node_id = operation.value("node_id", "");
    std::string dep_id = operation.value("depends_on_id", "");
    if (node_id.empty() || dep_id.empty()) {
      if (error) *error = "remove_dependency 缺少 node_id 或 depends_on_id";
      return false;
    }
    auto* node = FindNodeById(pipeline, node_id);
    if (!node) {
      if (error) *error = "NODE_NOT_FOUND: " + node_id;
      return false;
    }

    if (node->contains("depends_on") && (*node)["depends_on"].is_array()) {
      auto& deps = (*node)["depends_on"];
      for (auto it = deps.begin(); it != deps.end();) {
        if (it->is_string() && it->get<std::string>() == dep_id) {
          it = deps.erase(it);
        } else {
          ++it;
        }
      }
    }

    if (changes) {
      changes->push_back({"remove_dependency",
                          node_id,
                          "",
                          {node_id, dep_id},
                          "删除 " + node_id + " 对 " + dep_id + " 的执行依赖"});
    }
    return true;
  }

  if (error) *error = "UNKNOWN_OPERATION_KIND: " + kind;
  return false;
}

AuthoringResult PipelineAuthoring::ApplyRequest(const nlohmann::json& request) {
  AuthoringResult result;
  try {
    if (!request.is_object()) {
      result.ok = false;
      result.diagnostics.push_back("请求必须是 JSON 对象");
      return result;
    }

    // Check 4 MiB payload limit (RFC-0057 Section 4.4)
    if (request.dump().size() > 4 * 1024 * 1024) {
      result.ok = false;
      result.diagnostics.push_back(
          "REQUEST_TOO_LARGE: 请求输入大小超过单次上限 4 MiB");
      return result;
    }

    CheckFields(request, {"schema_version", "pipeline", "operation",
                          "operations", "require_valid"});
    if (request.contains("require_valid") &&
        !request["require_valid"].is_boolean())
      throw std::invalid_argument("require_valid 必须是布尔值");
    if (!request.contains("schema_version") ||
        !request["schema_version"].is_number_integer() ||
        request["schema_version"] != 1) {
      result.ok = false;
      result.diagnostics.push_back("schema_version 必须为 1");
      return result;
    }
    if (!request.contains("pipeline") || !request["pipeline"].is_object()) {
      result.ok = false;
      result.diagnostics.push_back("缺少 pipeline 对象");
      return result;
    }
    const auto& orig_pipeline = request["pipeline"];
    if (!orig_pipeline.contains("pipeline") ||
        !orig_pipeline["pipeline"].is_array()) {
      result.ok = false;
      result.diagnostics.push_back("pipeline 必须包含 pipeline 节点数组");
      return result;
    }

    bool has_op = request.contains("operation");
    bool has_ops = request.contains("operations");
    if ((has_op && has_ops) || (!has_op && !has_ops)) {
      result.ok = false;
      result.diagnostics.push_back(
          "请求必须包含互斥的 operation 或 operations");
      return result;
    }

    if ((has_op && !request["operation"].is_object()) ||
        (has_ops && !request["operations"].is_array()))
      throw std::invalid_argument(
          "operation 必须是对象，operations 必须是数组");
    std::vector<nlohmann::json> operations;
    if (has_op) {
      operations.push_back(request["operation"]);
    } else {
      operations = request["operations"].get<std::vector<nlohmann::json>>();
    }

    if (operations.empty()) {
      result.ok = false;
      result.diagnostics.push_back("operations 数组不能为空");
      return result;
    }
    if (operations.size() > 128) {
      result.ok = false;
      result.diagnostics.push_back("operations 超过单次上限 128");
      return result;
    }

    nlohmann::json working_pipeline = orig_pipeline;
    for (size_t i = 0; i < operations.size(); ++i) {
      result.failed_operation_index = i;
      std::string err;
      if (!ApplyOperation(&working_pipeline, operations[i], &result.changes,
                          &err)) {
        result.ok = false;
        result.failed_operation_index = i;
        result.diagnostics.push_back(err);
        result.pipeline = std::nullopt;
        return result;
      }
    }

    result.failed_operation_index.reset();
    auto val_res = ValidatePipelineDocument(working_pipeline,
                                            DocumentValidationMode::kExplain);
    result.validation = std::move(val_res.response);

    bool require_valid = request.value("require_valid", false);
    if (require_valid && !val_res.ok) {
      result.ok = false;
      result.pipeline = std::nullopt;
      return result;
    }

    result.ok = true;
    result.pipeline = std::move(working_pipeline);
    return result;
  } catch (const std::exception& error) {
    result.ok = false;
    result.pipeline.reset();
    result.changes.clear();
    result.diagnostics.push_back(error.what());
    return result;
  }
}

}  // namespace llm_edgeflow

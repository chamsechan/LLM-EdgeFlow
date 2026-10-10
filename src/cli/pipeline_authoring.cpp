#include "cli/pipeline_authoring.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "adapter/io_converter_registry.h"
#include "core/pipeline_catalog.h"
#include "pipeline_document_validation.h"

namespace llm_edgeflow {
namespace {

nlohmann::json* FindNode(nlohmann::json* pipeline, const std::string& name) {
  if (!pipeline || !pipeline->contains("pipeline") ||
      !(*pipeline)["pipeline"].is_array())
    return nullptr;
  nlohmann::json* found = nullptr;
  for (auto& node : (*pipeline)["pipeline"]) {
    if (node.is_object() && node.value("name", "") == name) {
      if (found) throw std::invalid_argument("AMBIGUOUS_NODE_NAME: " + name);
      found = &node;
    }
  }
  return found;
}

// 校验临时编辑协议，而非持久化的 Pipeline schema。
void CheckFields(const nlohmann::json& value,
                 std::initializer_list<const char*> allowed) {
  if (!value.is_object()) throw std::invalid_argument("字段必须是对象");
  for (const auto& item : value.items()) {
    if (std::none_of(allowed.begin(), allowed.end(),
                     [&](const char* key) { return item.key() == key; }))
      throw std::invalid_argument("UNKNOWN_FIELD: " + item.key());
  }
}

void RequireString(const nlohmann::json& value, const char* key) {
  if (!value.contains(key) || !value[key].is_string() ||
      value[key].get<std::string>().empty())
    throw std::invalid_argument(std::string("字段必须是非空字符串: ") + key);
}

void CheckNodeName(const std::string& name) {
  if (name == "input" || name == "output" ||
      name.find('.') != std::string::npos)
    throw std::invalid_argument("INVALID_NODE_NAME: " + name);
}

void CheckEndpoint(const nlohmann::json& endpoint) {
  CheckFields(endpoint, {"node", "port"});
  RequireString(endpoint, "node");
  RequireString(endpoint, "port");
}

void CheckOperation(const nlohmann::json& operation) {
  RequireString(operation, "kind");
  const auto kind = operation["kind"].get<std::string>();
  if (kind == "add_node") {
    CheckFields(operation, {"kind", "type", "name", "params"});
    RequireString(operation, "type");
    if (operation.contains("name")) RequireString(operation, "name");
    if (operation.contains("params") && !operation["params"].is_object())
      throw std::invalid_argument("params 必须是对象");
  } else if (kind == "remove_node" || kind == "rename_node") {
    if (kind == "remove_node")
      CheckFields(operation, {"kind", "node"});
    else {
      CheckFields(operation, {"kind", "node", "new_name"});
      RequireString(operation, "new_name");
    }
    RequireString(operation, "node");
  } else if (kind == "connect" || kind == "disconnect") {
    CheckFields(operation, {"kind", "source", "target"});
    CheckEndpoint(operation.at("source"));
    CheckEndpoint(operation.at("target"));
  } else if (kind == "add_dependency" || kind == "remove_dependency") {
    CheckFields(operation, {"kind", "node", "depends_on"});
    RequireString(operation, "node");
    RequireString(operation, "depends_on");
  } else {
    throw std::invalid_argument("UNKNOWN_OPERATION_KIND: " + kind);
  }
}

struct ResolvedEndpoint {
  std::string node;
  std::string port;
  PortContract contract;
  // 对输入端口，指向拥有 inputs 的节点或输出项；源端口不修改文档。
  nlohmann::json* consumer = nullptr;

  std::string Source() const { return node + "." + port; }
};

ResolvedEndpoint ResolveEndpoint(nlohmann::json* pipeline,
                                 const nlohmann::json& endpoint, bool source) {
  ResolvedEndpoint result;
  result.node = endpoint.at("node").get<std::string>();
  result.port = endpoint.at("port").get<std::string>();
  if (result.node != "input" && result.node != "output") {
    auto* node = FindNode(pipeline, result.node);
    if (!node) throw std::invalid_argument("NODE_NOT_FOUND: " + result.node);
    const auto type = node->value("type", "");
    const auto definition = PipelineCatalog::FindNode(type);
    if (!definition) throw std::invalid_argument("UNKNOWN_NODE_TYPE: " + type);
    const auto& ports = source ? definition->outputs : definition->inputs;
    const auto port = std::find_if(
        ports.begin(), ports.end(),
        [&](const auto& item) { return item.logical_name == result.port; });
    if (port == ports.end())
      throw std::invalid_argument("UNKNOWN_PORT: " + result.Source());
    result.contract = *port;
    if (!source) result.consumer = node;
    return result;
  }

  if ((source && result.node != "input") ||
      (!source && result.node != "output"))
    throw std::invalid_argument("INVALID_ENDPOINT: input/output 端点方向错误");
  const auto io = pipeline->find("io");
  if (io == pipeline->end() || !io->is_object())
    throw std::invalid_argument("UNKNOWN_PORT: " + result.Source());
  const char* direction = source ? "input" : "output";
  const auto entries = io->find(direction);
  if (entries == io->end() || !entries->is_array())
    throw std::invalid_argument("UNKNOWN_PORT: " + result.Source());
  nlohmann::json pairs = nlohmann::json::array();
  const auto& registry = IoConverterRegistry::Instance();
  for (auto& entry : *entries) {
    RequireString(entry, "type");
    RequireString(entry, "name");
    const auto type = entry["type"].get<std::string>();
    const auto name = entry["name"].get<std::string>();
    const std::vector<NodePortDefinition>* ports = nullptr;
    if (source) {
      const auto* converter = registry.FindInputConverter(type, name);
      if (converter) ports = &converter->logical_ports;
    } else {
      const auto* converter = registry.FindOutputConverter(type, name);
      if (converter) ports = &converter->logical_ports;
    }
    if (!ports)
      throw std::invalid_argument("UNKNOWN_CONVERTER: " + type + "/" + name);
    for (const auto& port : *ports) {
      if (port.logical_name != result.port) continue;
      pairs.push_back({{"type", type}, {"name", name}});
      result.contract = port;
      if (!source) result.consumer = &entry;
    }
  }
  if (pairs.empty())
    throw std::invalid_argument("UNKNOWN_PORT: " + result.Source());
  if (pairs.size() > 1)
    throw std::invalid_argument(std::string(source
                                                ? "DUPLICATE_PORT_PRODUCER: "
                                                : "AMBIGUOUS_OUTPUT_PORT: ") +
                                result.Source() + " " + pairs.dump());
  return result;
}

nlohmann::json& Inputs(nlohmann::json* consumer) {
  if (!consumer->contains("inputs"))
    (*consumer)["inputs"] = nlohmann::json::object();
  if (!(*consumer)["inputs"].is_object())
    throw std::invalid_argument("inputs 必须是对象");
  return (*consumer)["inputs"];
}

// 引用只按源节点前缀改写，保留其他节点的输入与显式顺序约束。
bool RewriteInputs(nlohmann::json* item, const std::string& name,
                   const std::optional<std::string>& replacement) {
  if (!item->contains("inputs") || !(*item)["inputs"].is_object()) return false;
  auto& inputs = (*item)["inputs"];
  const auto prefix = name + ".";
  bool changed = false;
  for (auto it = inputs.begin(); it != inputs.end();) {
    if (it->is_string() && it->get_ref<const std::string&>().compare(
                               0, prefix.size(), prefix) == 0) {
      if (replacement) {
        *it = *replacement + it->get<std::string>().substr(name.size());
        ++it;
      } else {
        it = inputs.erase(it);
      }
      changed = true;
    } else {
      ++it;
    }
  }
  return changed;
}

std::vector<std::string> RewriteReferences(
    nlohmann::json* pipeline, const std::string& name,
    const std::optional<std::string>& replacement) {
  std::vector<std::string> affected;
  for (auto& node : (*pipeline)["pipeline"]) {
    if (!node.is_object()) continue;
    bool changed = RewriteInputs(&node, name, replacement);
    if (node.contains("depends_on") && node["depends_on"].is_array()) {
      auto& dependencies = node["depends_on"];
      for (auto it = dependencies.begin(); it != dependencies.end();) {
        if (it->is_string() && it->get<std::string>() == name) {
          if (replacement) {
            *it = *replacement;
            ++it;
          } else {
            it = dependencies.erase(it);
          }
          changed = true;
        } else {
          ++it;
        }
      }
    }
    if (changed) affected.push_back(node.value("name", ""));
  }
  if (pipeline->contains("io") && (*pipeline)["io"].is_object() &&
      (*pipeline)["io"].contains("output") &&
      (*pipeline)["io"]["output"].is_array()) {
    bool changed = false;
    for (auto& entry : (*pipeline)["io"]["output"])
      if (entry.is_object())
        changed = RewriteInputs(&entry, name, replacement) || changed;
    if (changed) affected.push_back("output");
  }
  return affected;
}

bool ApplyMutation(nlohmann::json* pipeline, const nlohmann::json& operation,
                   std::vector<AuthoringChange>* changes, std::string* error) {
  const auto kind = operation["kind"].get<std::string>();
  if (kind == "add_node") {
    const auto type = operation["type"].get<std::string>();
    if (!PipelineCatalog::FindNode(type))
      throw std::invalid_argument("UNKNOWN_NODE_TYPE: " + type);
    std::string name = operation.value("name", type);
    CheckNodeName(name);
    if (operation.contains("name")) {
      if (FindNode(pipeline, name))
        throw std::invalid_argument("DUPLICATE_NODE_NAME: " + name);
    } else {
      for (size_t suffix = 2; FindNode(pipeline, name); ++suffix)
        name = type + "_" + std::to_string(suffix);
    }
    nlohmann::json node = {{"type", type}, {"name", name}};
    if (operation.contains("params")) node["params"] = operation["params"];
    (*pipeline)["pipeline"].push_back(std::move(node));
    changes->push_back(
        {kind, name, "", {name}, "添加节点 " + name + " (" + type + ")"});
    return true;
  }

  if (kind == "remove_node" || kind == "rename_node") {
    const auto name = operation["node"].get<std::string>();
    auto* node = FindNode(pipeline, name);
    if (!node) throw std::invalid_argument("NODE_NOT_FOUND: " + name);
    if (kind == "rename_node") {
      const auto replacement = operation["new_name"].get<std::string>();
      CheckNodeName(replacement);
      if (replacement != name && FindNode(pipeline, replacement))
        throw std::invalid_argument("DUPLICATE_NODE_NAME: " + replacement);
      (*node)["name"] = replacement;
      auto affected = RewriteReferences(pipeline, name, replacement);
      if (std::find(affected.begin(), affected.end(), replacement) ==
          affected.end())
        affected.insert(affected.begin(), replacement);
      changes->push_back({kind, replacement, "", std::move(affected),
                          "重命名节点 " + name + " 为 " + replacement});
    } else {
      auto affected = RewriteReferences(pipeline, name, std::nullopt);
      if (std::find(affected.begin(), affected.end(), name) == affected.end())
        affected.insert(affected.begin(), name);
      auto& nodes = (*pipeline)["pipeline"];
      const auto item =
          std::find_if(nodes.begin(), nodes.end(), [&](const auto& candidate) {
            return candidate.is_object() && candidate.value("name", "") == name;
          });
      nodes.erase(item);
      changes->push_back(
          {kind, name, "", std::move(affected), "删除节点 " + name});
    }
    return true;
  }

  if (kind == "connect" || kind == "disconnect") {
    const auto source = ResolveEndpoint(pipeline, operation["source"], true);
    const auto target = ResolveEndpoint(pipeline, operation["target"], false);
    if (kind == "connect") {
      if (source.contract.type_id != target.contract.type_id)
        throw std::invalid_argument(
            "PORT_TYPE_MISMATCH: " + source.contract.type_id +
            " != " + target.contract.type_id);
      Inputs(target.consumer)[target.port] = source.Source();
    } else {
      const auto bindings = target.consumer->find("inputs");
      if (bindings == target.consumer->end() || !bindings->is_object() ||
          !bindings->contains(target.port) ||
          !(*bindings)[target.port].is_string() ||
          (*bindings)[target.port].get<std::string>() != source.Source()) {
        if (error) *error = "连线不存在或已被修改";
        return false;
      }
      bindings->erase(target.port);
    }
    changes->push_back({kind,
                        target.node,
                        target.port,
                        {source.node, target.node},
                        (kind == "connect" ? "连接 " : "断开 ") +
                            source.Source() + " 到 " + target.Source()});
    return true;
  }

  const auto name = operation["node"].get<std::string>();
  const auto dependency = operation["depends_on"].get<std::string>();
  auto* node = FindNode(pipeline, name);
  if (!node) throw std::invalid_argument("NODE_NOT_FOUND: " + name);
  if (kind == "add_dependency") {
    if (!FindNode(pipeline, dependency))
      throw std::invalid_argument("NODE_NOT_FOUND: " + dependency);
    if (!node->contains("depends_on"))
      (*node)["depends_on"] = nlohmann::json::array();
    if (!(*node)["depends_on"].is_array())
      throw std::invalid_argument("depends_on 必须是数组");
    auto& dependencies = (*node)["depends_on"];
    if (std::find(dependencies.begin(), dependencies.end(), dependency) ==
        dependencies.end())
      dependencies.push_back(dependency);
  } else if (node->contains("depends_on") && (*node)["depends_on"].is_array()) {
    auto& dependencies = (*node)["depends_on"];
    for (auto it = dependencies.begin(); it != dependencies.end();)
      if (it->is_string() && it->get<std::string>() == dependency)
        it = dependencies.erase(it);
      else
        ++it;
  }
  changes->push_back({kind,
                      name,
                      "",
                      {name, dependency},
                      (kind == "add_dependency" ? "添加 " : "删除 ") + name +
                          " 对 " + dependency + " 的执行依赖"});
  return true;
}

}  // namespace

nlohmann::json AuthoringChange::ToJson() const {
  nlohmann::json j = {{"action", action},
                      {"node_name", node_name},
                      {"affected_nodes", affected_nodes},
                      {"description", description}};
  if (!port.empty()) {
    j["port"] = port;
  }
  return j;
}

nlohmann::json AuthoringResult::ToJson() const {
  nlohmann::json j = {{"ok", ok}};
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

bool PipelineAuthoring::ApplyOperation(nlohmann::json* pipeline,
                                       const nlohmann::json& operation,
                                       std::vector<AuthoringChange>* changes,
                                       std::string* error) {
  if (!pipeline || !pipeline->is_object() || !operation.is_object() ||
      !pipeline->contains("pipeline") || !(*pipeline)["pipeline"].is_array()) {
    if (error) *error = "无效的 pipeline 或 operation 对象";
    return false;
  }
  CheckOperation(operation);
  nlohmann::json candidate = *pipeline;
  std::vector<AuthoringChange> staged_changes;
  if (!ApplyMutation(&candidate, operation, &staged_changes, error))
    return false;
  const auto kind = operation["kind"].get<std::string>();
  if (kind == "connect" || kind == "add_dependency") {
    const auto validation =
        ValidatePipelineDocument(candidate, DocumentValidationMode::kValidate);
    if (validation.core_report) {
      const auto cycle =
          std::find_if(validation.core_report->diagnostics.begin(),
                       validation.core_report->diagnostics.end(),
                       [](const auto& diagnostic) {
                         return diagnostic.code == DiagnosticCode::kDagCycle;
                       });
      if (cycle != validation.core_report->diagnostics.end()) {
        if (error) *error = "DAG_CYCLE: " + cycle->message;
        return false;
      }
    }
  }
  if (changes)
    changes->insert(changes->end(), staged_changes.begin(),
                    staged_changes.end());
  *pipeline = std::move(candidate);
  return true;
}

AuthoringResult PipelineAuthoring::ApplyRequest(const nlohmann::json& request) {
  AuthoringResult result;
  try {
    if (!request.is_object()) {
      result.ok = false;
      result.diagnostics.push_back("请求必须是 JSON 对象");
      return result;
    }

    if (request.dump().size() > 4 * 1024 * 1024) {
      result.ok = false;
      result.diagnostics.push_back(
          "REQUEST_TOO_LARGE: 请求输入大小超过单次上限 4 MiB");
      return result;
    }

    CheckFields(request,
                {"pipeline", "operation", "operations", "require_valid"});
    if (request.contains("require_valid") &&
        !request["require_valid"].is_boolean())
      throw std::invalid_argument("require_valid 必须是布尔值");
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

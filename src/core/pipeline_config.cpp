#include "core/pipeline_config.h"

#include <algorithm>
#include <string_view>
#include <tuple>
#include <unordered_set>

#include "contracts/json_pointer.h"
#include "contracts/json_structure.h"
#include "pipeline_config_structure.h"

namespace llm_edgeflow {
namespace shape = json_structure;

namespace {

void SetDiag(PipelineDiagnostic* diag, DiagnosticCode code,
             const std::string& path, const std::string& message) {
  if (diag) {
    diag->code = code;
    diag->path = path;
    diag->message = message;
  }
}

}  // namespace

bool ParsePipelineConfig(const nlohmann::json& root,
                         ParsedPipelineConfig* output,
                         PipelineDiagnostic* diagnostic) {
  if (diagnostic) {
    diagnostic->Clear();
  }
  if (!output) {
    SetDiag(diagnostic, DiagnosticCode::kFieldType, "/", "Null output pointer");
    return false;
  }

  const auto& structure = PipelineConfigStructure();
  const auto& models_shape = shape::Property(structure, "models");
  const auto& model_shape = models_shape.at("items");
  const auto& nodes_shape = shape::Property(structure, "pipeline");
  const auto& node_shape = nodes_shape.at("items");

  // 1. 根节点必须是 JSON Object
  if (!shape::HasType(root, structure)) {
    SetDiag(diagnostic, DiagnosticCode::kRootType, "/",
            "Pipeline configuration root must be a JSON object");
    return false;
  }

  // 2. 拒绝根节点未知字段
  for (auto it = root.begin(); it != root.end(); ++it) {
    if (!shape::AllowsProperty(structure, it.key())) {
      SetDiag(diagnostic, DiagnosticCode::kUnknownField, "/" + it.key(),
              "Unknown root field: " + it.key());
      return false;
    }
  }

  // comment (可选字符串)
  if (root.contains("comment") &&
      !shape::HasType(root["comment"], shape::Property(structure, "comment"))) {
    SetDiag(diagnostic, DiagnosticCode::kFieldType, "/comment",
            "Field 'comment' must be a string");
    return false;
  }

  ParsedPipelineConfig result;

  // 同一个 worker 预算同时控制串行和并行执行。
  if (root.contains("max_parallel_workers")) {
    if (!shape::HasType(root["max_parallel_workers"],
                        shape::Property(structure, "max_parallel_workers"))) {
      SetDiag(diagnostic, DiagnosticCode::kFieldType, "/max_parallel_workers",
              "Field 'max_parallel_workers' must be an integer");
      return false;
    }
    int64_t workers = root["max_parallel_workers"].get<int64_t>();
    if (shape::BelowMinimum(
            workers, shape::Property(structure, "max_parallel_workers")) ||
        shape::AboveMaximum(
            workers, shape::Property(structure, "max_parallel_workers"))) {
      SetDiag(diagnostic, DiagnosticCode::kFieldRange, "/max_parallel_workers",
              "Field 'max_parallel_workers' must be between 1 and 64");
      return false;
    }
    result.max_parallel_workers = static_cast<size_t>(workers);
  } else {
    result.max_parallel_workers = 1;
  }

  // 6. 解析 models: 可选数组，最多 64 个模型定义
  if (root.contains("models")) {
    if (!shape::HasType(root["models"], shape::Property(structure, "models"))) {
      SetDiag(diagnostic, DiagnosticCode::kFieldType, "/models",
              "Field 'models' must be an array");
      return false;
    }
    if (shape::TooLong(root["models"], models_shape)) {
      SetDiag(diagnostic, DiagnosticCode::kFieldRange, "/models",
              "Model count exceeds maximum limit of 64");
      return false;
    }

    std::unordered_set<std::string> seen_model_names;
    const auto& backend_shape = shape::Property(model_shape, "backend");
    for (size_t i = 0; i < root["models"].size(); ++i) {
      const auto& item = root["models"][i];
      const auto at = "/models/" + std::to_string(i);
      const auto check_object = [&](const nlohmann::json& object,
                                    const nlohmann::json& declaration,
                                    const std::string& path) {
        if (!object.is_object()) {
          SetDiag(diagnostic, DiagnosticCode::kFieldType, path,
                  "Expected an object");
          return false;
        }
        for (const auto& [key, value] : object.items()) {
          if (!shape::AllowsProperty(declaration, key)) {
            SetDiag(diagnostic, DiagnosticCode::kUnknownField,
                    path + "/" + EscapeJsonPointer(key),
                    "Unknown field: " + key);
            return false;
          }
        }
        for (const auto& key : declaration.at("required")) {
          const auto field = key.get<std::string>();
          if (!object.contains(field)) {
            SetDiag(diagnostic, DiagnosticCode::kMissingField,
                    path + "/" + field,
                    "Missing required field '" + field + "'");
            return false;
          }
        }
        return true;
      };
      if (!check_object(item, model_shape, at)) return false;
      const auto read_string = [&](const nlohmann::json& object,
                                   const std::string& field,
                                   const std::string& path,
                                   std::string* value) {
        if (!object[field].is_string()) {
          SetDiag(diagnostic, DiagnosticCode::kFieldType, path + "/" + field,
                  "Field '" + field + "' must be a string");
          return false;
        }
        *value = object[field].get<std::string>();
        if (value->empty()) {
          SetDiag(diagnostic, DiagnosticCode::kFieldRange, path + "/" + field,
                  "Field '" + field + "' cannot be empty");
          return false;
        }
        return true;
      };
      ParsedModelConfig model;
      model.source_index = i;
      if (!read_string(item, "type", at, &model.model_type) ||
          !read_string(item, "name", at, &model.model_name) ||
          !read_string(item, "file", at, &model.model_file) ||
          !check_object(item["backend"], backend_shape, at + "/backend") ||
          !read_string(item["backend"], "type", at + "/backend",
                       &model.backend_type))
        return false;
      if (!seen_model_names.insert(model.model_name).second) {
        SetDiag(diagnostic, DiagnosticCode::kDuplicateModelName, at + "/name",
                "Duplicate model name: " + model.model_name);
        return false;
      }
      for (const auto& [object, path, params] :
           {std::tuple<const nlohmann::json*, std::string, nlohmann::json*>{
                &item, at, &model.model_params},
            {&item["backend"], at + "/backend", &model.backend_params}}) {
        if (object->contains("params")) {
          if (!(*object)["params"].is_object()) {
            SetDiag(diagnostic, DiagnosticCode::kFieldType, path + "/params",
                    "Field 'params' must be an object");
            return false;
          }
          *params = (*object)["params"];
        }
      }
      result.models.push_back(std::move(model));
    }
  }

  // 7. 解析 pipeline: 必填非空数组，最多 256 个节点定义
  if (shape::MissingRequired(root, structure, "pipeline")) {
    SetDiag(diagnostic, DiagnosticCode::kMissingField, "/pipeline",
            "Missing required field 'pipeline'");
    return false;
  }
  if (!shape::HasType(root["pipeline"],
                      shape::Property(structure, "pipeline"))) {
    SetDiag(diagnostic, DiagnosticCode::kFieldType, "/pipeline",
            "Field 'pipeline' must be an array");
    return false;
  }
  if (shape::TooShort(root["pipeline"], nodes_shape)) {
    SetDiag(diagnostic, DiagnosticCode::kFieldRange, "/pipeline",
            "Pipeline cannot be empty");
    return false;
  }
  if (shape::TooLong(root["pipeline"], nodes_shape)) {
    SetDiag(diagnostic, DiagnosticCode::kFieldRange, "/pipeline",
            "Pipeline node count exceeds maximum limit of 256");
    return false;
  }

  std::unordered_set<std::string> seen_node_ids;

  for (size_t i = 0; i < root["pipeline"].size(); ++i) {
    const auto& node_elem = root["pipeline"][i];
    std::string node_path_prefix = "/pipeline/" + std::to_string(i);

    if (!shape::HasType(node_elem, node_shape)) {
      SetDiag(diagnostic, DiagnosticCode::kFieldType, node_path_prefix,
              "Node item must be an object");
      return false;
    }

    // 拒绝 node 内部未知字段
    for (auto it = node_elem.begin(); it != node_elem.end(); ++it) {
      if (!shape::AllowsProperty(node_shape, it.key())) {
        SetDiag(diagnostic, DiagnosticCode::kUnknownField,
                node_path_prefix + "/" + it.key(),
                "Unknown field in node: " + it.key());
        return false;
      }
    }

    // comment 字段类型检查 (R1-ACC-006)
    if (node_elem.contains("comment") &&
        !shape::HasType(node_elem["comment"],
                        shape::Property(node_shape, "comment"))) {
      SetDiag(diagnostic, DiagnosticCode::kFieldType,
              node_path_prefix + "/comment",
              "Field 'comment' must be a string");
      return false;
    }

    ParsedNodeConfig node_cfg;
    node_cfg.source_index = i;

    // node_type (必填非空字符串)
    if (shape::MissingRequired(node_elem, node_shape, "node_type")) {
      SetDiag(diagnostic, DiagnosticCode::kMissingField,
              node_path_prefix + "/node_type",
              "Missing required field 'node_type'");
      return false;
    }
    if (!shape::HasType(node_elem["node_type"],
                        shape::Property(node_shape, "node_type"))) {
      SetDiag(diagnostic, DiagnosticCode::kFieldType,
              node_path_prefix + "/node_type",
              "Field 'node_type' must be a string");
      return false;
    }
    node_cfg.node_type = node_elem["node_type"].get<std::string>();
    if (shape::TooShort(node_elem["node_type"],
                        shape::Property(node_shape, "node_type"))) {
      SetDiag(diagnostic, DiagnosticCode::kFieldRange,
              node_path_prefix + "/node_type",
              "Field 'node_type' cannot be empty");
      return false;
    }

    // 输入连接和输出名称使用相同的显式映射结构。
    for (const char* direction : {"inputs", "outputs"}) {
      if (!node_elem.contains(direction)) continue;
      const auto& bindings = node_elem[direction];
      const auto& mapping_shape = shape::Property(node_shape, direction);
      const std::string mapping_path = node_path_prefix + "/" + direction;
      if (!shape::HasType(bindings, mapping_shape)) {
        SetDiag(diagnostic, DiagnosticCode::kFieldType, mapping_path,
                std::string("Field '") + direction + "' must be an object");
        return false;
      }
      auto& targets = std::string_view(direction) == "inputs"
                          ? node_cfg.ports.inputs
                          : node_cfg.ports.outputs;
      for (auto it = bindings.begin(); it != bindings.end(); ++it) {
        const auto& target_shape = mapping_shape.at("additionalProperties");
        if (!shape::HasType(it.value(), target_shape)) {
          SetDiag(diagnostic, DiagnosticCode::kFieldType,
                  mapping_path + "/" + it.key(),
                  "Port mapping target must be a string");
          return false;
        }
        if (shape::TooShort(it.value(), target_shape)) {
          SetDiag(diagnostic, DiagnosticCode::kFieldRange,
                  mapping_path + "/" + it.key(),
                  "Port mapping target cannot be empty");
          return false;
        }
        targets[it.key()] = it.value().get<std::string>();
      }
    }

    // config (可选对象)
    if (node_elem.contains("config")) {
      if (!shape::HasType(node_elem["config"],
                          shape::Property(node_shape, "config"))) {
        SetDiag(diagnostic, DiagnosticCode::kFieldType,
                node_path_prefix + "/config",
                "Field 'config' must be an object");
        return false;
      }
      node_cfg.config = node_elem["config"];
    } else {
      node_cfg.config = nlohmann::json::object();
    }

    // id (必填非空字符串，唯一)
    if (shape::MissingRequired(node_elem, node_shape, "id")) {
      SetDiag(diagnostic, DiagnosticCode::kMissingField,
              node_path_prefix + "/id",
              "Missing required field 'id' in pipeline node");
      return false;
    }
    if (!shape::HasType(node_elem["id"], shape::Property(node_shape, "id"))) {
      SetDiag(diagnostic, DiagnosticCode::kFieldType, node_path_prefix + "/id",
              "Field 'id' must be a string");
      return false;
    }
    node_cfg.id = node_elem["id"].get<std::string>();
    if (shape::TooShort(node_elem["id"], shape::Property(node_shape, "id"))) {
      SetDiag(diagnostic, DiagnosticCode::kFieldRange, node_path_prefix + "/id",
              "Field 'id' cannot be empty");
      return false;
    }
    if (seen_node_ids.find(node_cfg.id) != seen_node_ids.end()) {
      SetDiag(diagnostic, DiagnosticCode::kDuplicateNodeId,
              node_path_prefix + "/id", "Duplicate node id: " + node_cfg.id);
      return false;
    }
    seen_node_ids.insert(node_cfg.id);

    // 可选的附加顺序约束。数据依赖由 PipelineValidator
    // 根据输入/输出绑定规划。
    if (node_elem.contains("depends_on")) {
      if (!shape::HasType(node_elem["depends_on"],
                          shape::Property(node_shape, "depends_on"))) {
        SetDiag(diagnostic, DiagnosticCode::kFieldType,
                node_path_prefix + "/depends_on",
                "Field 'depends_on' must be an array");
        return false;
      }
      if (shape::TooLong(node_elem["depends_on"],
                         shape::Property(node_shape, "depends_on"))) {
        SetDiag(diagnostic, DiagnosticCode::kFieldRange,
                node_path_prefix + "/depends_on",
                "Node dependencies exceed limit of 256");
        return false;
      }

      for (size_t d = 0; d < node_elem["depends_on"].size(); ++d) {
        const auto& dep_item = node_elem["depends_on"][d];
        std::string dep_path =
            node_path_prefix + "/depends_on/" + std::to_string(d);

        if (!shape::HasType(
                dep_item,
                shape::Property(node_shape, "depends_on").at("items"))) {
          SetDiag(diagnostic, DiagnosticCode::kFieldType, dep_path,
                  "Dependency item must be a string");
          return false;
        }
        std::string dep_str = dep_item.get<std::string>();
        if (shape::TooShort(
                dep_item,
                shape::Property(node_shape, "depends_on").at("items"))) {
          SetDiag(diagnostic, DiagnosticCode::kFieldRange, dep_path,
                  "Dependency item cannot be empty");
          return false;
        }
        node_cfg.depends_on.push_back(dep_str);
      }
    }

    result.nodes.push_back(std::move(node_cfg));
  }

  *output = std::move(result);
  return true;
}

}  // namespace llm_edgeflow

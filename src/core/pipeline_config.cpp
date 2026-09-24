#include "core/pipeline_config.h"

#include <algorithm>
#include <string_view>
#include <unordered_set>

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

  // 3. biz_name 是 v6 唯一业务标识字段，必须存在且为非空字符串。
  if (shape::MissingRequired(root, structure, "biz_name")) {
    SetDiag(diagnostic, DiagnosticCode::kMissingField, "/biz_name",
            "Missing required field 'biz_name'");
    return false;
  }
  if (!shape::HasType(root["biz_name"],
                      shape::Property(structure, "biz_name"))) {
    SetDiag(diagnostic, DiagnosticCode::kFieldType, "/biz_name",
            "Field 'biz_name' must be a string");
    return false;
  }
  result.biz_name = root["biz_name"].get<std::string>();
  if (shape::TooShort(root["biz_name"],
                      shape::Property(structure, "biz_name"))) {
    SetDiag(diagnostic, DiagnosticCode::kFieldRange, "/biz_name",
            "Field 'biz_name' cannot be empty");
    return false;
  }

  // A single worker budget controls sequential and parallel execution.
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

    std::unordered_set<std::string> seen_model_ids;

    for (size_t i = 0; i < root["models"].size(); ++i) {
      const auto& model_elem = root["models"][i];
      std::string model_path_prefix = "/models/" + std::to_string(i);

      if (!shape::HasType(model_elem, model_shape)) {
        SetDiag(diagnostic, DiagnosticCode::kFieldType, model_path_prefix,
                "Model item must be an object");
        return false;
      }

      // 拒绝 model 内部未知字段
      for (auto it = model_elem.begin(); it != model_elem.end(); ++it) {
        if (!shape::AllowsProperty(model_shape, it.key())) {
          SetDiag(diagnostic, DiagnosticCode::kUnknownField,
                  model_path_prefix + "/" + it.key(),
                  "Unknown field in model: " + it.key());
          return false;
        }
      }

      // comment 字段类型检查 (R1-ACC-006)
      if (model_elem.contains("comment") &&
          !shape::HasType(model_elem["comment"],
                          shape::Property(model_shape, "comment"))) {
        SetDiag(diagnostic, DiagnosticCode::kFieldType,
                model_path_prefix + "/comment",
                "Field 'comment' must be a string");
        return false;
      }

      ParsedModelConfig model_cfg;
      model_cfg.source_index = i;

      // model_id (必填非空字符串，唯一)
      if (shape::MissingRequired(model_elem, model_shape, "model_id")) {
        SetDiag(diagnostic, DiagnosticCode::kMissingField,
                model_path_prefix + "/model_id",
                "Missing required field 'model_id'");
        return false;
      }
      if (!shape::HasType(model_elem["model_id"],
                          shape::Property(model_shape, "model_id"))) {
        SetDiag(diagnostic, DiagnosticCode::kFieldType,
                model_path_prefix + "/model_id",
                "Field 'model_id' must be a string");
        return false;
      }
      model_cfg.model_id = model_elem["model_id"].get<std::string>();
      if (shape::TooShort(model_elem["model_id"],
                          shape::Property(model_shape, "model_id"))) {
        SetDiag(diagnostic, DiagnosticCode::kFieldRange,
                model_path_prefix + "/model_id",
                "Field 'model_id' cannot be empty");
        return false;
      }
      if (seen_model_ids.find(model_cfg.model_id) != seen_model_ids.end()) {
        SetDiag(diagnostic, DiagnosticCode::kDuplicateModelId,
                model_path_prefix + "/model_id",
                "Duplicate model_id: " + model_cfg.model_id);
        return false;
      }
      seen_model_ids.insert(model_cfg.model_id);

      // model_type (必填非空字符串)
      if (shape::MissingRequired(model_elem, model_shape, "model_type")) {
        SetDiag(diagnostic, DiagnosticCode::kMissingField,
                model_path_prefix + "/model_type",
                "Missing required field 'model_type'");
        return false;
      }
      if (!shape::HasType(model_elem["model_type"],
                          shape::Property(model_shape, "model_type"))) {
        SetDiag(diagnostic, DiagnosticCode::kFieldType,
                model_path_prefix + "/model_type",
                "Field 'model_type' must be a string");
        return false;
      }
      model_cfg.model_type = model_elem["model_type"].get<std::string>();
      if (shape::TooShort(model_elem["model_type"],
                          shape::Property(model_shape, "model_type"))) {
        SetDiag(diagnostic, DiagnosticCode::kFieldRange,
                model_path_prefix + "/model_type",
                "Field 'model_type' cannot be empty");
        return false;
      }

      // backend (必填非空字符串)
      if (shape::MissingRequired(model_elem, model_shape, "backend")) {
        SetDiag(diagnostic, DiagnosticCode::kMissingField,
                model_path_prefix + "/backend",
                "Missing required field 'backend'");
        return false;
      }
      if (!shape::HasType(model_elem["backend"],
                          shape::Property(model_shape, "backend"))) {
        SetDiag(diagnostic, DiagnosticCode::kFieldType,
                model_path_prefix + "/backend",
                "Field 'backend' must be a string");
        return false;
      }
      model_cfg.backend = model_elem["backend"].get<std::string>();
      if (shape::TooShort(model_elem["backend"],
                          shape::Property(model_shape, "backend"))) {
        SetDiag(diagnostic, DiagnosticCode::kFieldRange,
                model_path_prefix + "/backend",
                "Field 'backend' cannot be empty");
        return false;
      }

      // model_path (必填非空字符串)
      if (shape::MissingRequired(model_elem, model_shape, "model_path")) {
        SetDiag(diagnostic, DiagnosticCode::kMissingField,
                model_path_prefix + "/model_path",
                "Missing required field 'model_path'");
        return false;
      }
      if (!shape::HasType(model_elem["model_path"],
                          shape::Property(model_shape, "model_path"))) {
        SetDiag(diagnostic, DiagnosticCode::kFieldType,
                model_path_prefix + "/model_path",
                "Field 'model_path' must be a string");
        return false;
      }
      model_cfg.model_path = model_elem["model_path"].get<std::string>();
      if (shape::TooShort(model_elem["model_path"],
                          shape::Property(model_shape, "model_path"))) {
        SetDiag(diagnostic, DiagnosticCode::kFieldRange,
                model_path_prefix + "/model_path",
                "Field 'model_path' cannot be empty");
        return false;
      }

      // model_config (可选对象)
      if (model_elem.contains("model_config")) {
        if (!shape::HasType(model_elem["model_config"],
                            shape::Property(model_shape, "model_config"))) {
          SetDiag(diagnostic, DiagnosticCode::kFieldType,
                  model_path_prefix + "/model_config",
                  "Field 'model_config' must be an object");
          return false;
        }
        model_cfg.model_config = model_elem["model_config"];
      } else {
        model_cfg.model_config = nlohmann::json::object();
      }

      // backend_config (可选对象)
      if (model_elem.contains("backend_config")) {
        if (!shape::HasType(model_elem["backend_config"],
                            shape::Property(model_shape, "backend_config"))) {
          SetDiag(diagnostic, DiagnosticCode::kFieldType,
                  model_path_prefix + "/backend_config",
                  "Field 'backend_config' must be an object");
          return false;
        }
        model_cfg.backend_config = model_elem["backend_config"];
      } else {
        model_cfg.backend_config = nlohmann::json::object();
      }
      result.models.push_back(std::move(model_cfg));
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

    // Input connections and output names use the same explicit mapping shape.
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

    // Optional additional ordering constraints. Data dependencies are planned
    // by PipelineValidator from input/output bindings.
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

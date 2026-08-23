#include "core/pipeline_config.h"

#include <algorithm>
#include <unordered_set>

namespace alg_framework {

namespace {

void SetDiag(PipelineDiagnostic* diag, PipelineErrorCode code,
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
    SetDiag(diagnostic, PipelineErrorCode::kFieldType, "/",
            "Null output pointer");
    return false;
  }

  // 1. 根节点必须是 JSON Object
  if (!root.is_object()) {
    SetDiag(diagnostic, PipelineErrorCode::kRootType, "/",
            "Pipeline configuration root must be a JSON object");
    return false;
  }

  // 2. 拒绝根节点未知字段
  const std::unordered_set<std::string> allowed_root_keys = {
      "business_name",        "models", "pipeline", "execution_mode",
      "max_parallel_workers", "comment"};
  for (auto it = root.begin(); it != root.end(); ++it) {
    if (allowed_root_keys.find(it.key()) == allowed_root_keys.end()) {
      SetDiag(diagnostic, PipelineErrorCode::kUnknownField, "/" + it.key(),
              "Unknown root field: " + it.key());
      return false;
    }
  }

  // comment (可选字符串)
  if (root.contains("comment") && !root["comment"].is_string()) {
    SetDiag(diagnostic, PipelineErrorCode::kFieldType, "/comment",
            "Field 'comment' must be a string");
    return false;
  }

  ParsedPipelineConfig result;

  // 3. 解析 business_name: 必须存在且为非空字符串
  if (!root.contains("business_name")) {
    SetDiag(diagnostic, PipelineErrorCode::kMissingField, "/business_name",
            "Missing required field 'business_name'");
    return false;
  }
  if (!root["business_name"].is_string()) {
    SetDiag(diagnostic, PipelineErrorCode::kFieldType, "/business_name",
            "Field 'business_name' must be a string");
    return false;
  }
  result.business_name = root["business_name"].get<std::string>();
  if (result.business_name.empty()) {
    SetDiag(diagnostic, PipelineErrorCode::kFieldRange, "/business_name",
            "Field 'business_name' cannot be empty");
    return false;
  }

  // 4. 解析 execution_mode: 可选字符串，仅支持 "sequential" 与 "parallel"
  if (root.contains("execution_mode")) {
    if (!root["execution_mode"].is_string()) {
      SetDiag(diagnostic, PipelineErrorCode::kFieldType, "/execution_mode",
              "Field 'execution_mode' must be a string");
      return false;
    }
    std::string mode_str = root["execution_mode"].get<std::string>();
    if (mode_str != "sequential" && mode_str != "parallel") {
      SetDiag(diagnostic, PipelineErrorCode::kFieldRange, "/execution_mode",
              "Field 'execution_mode' must be 'sequential' or 'parallel'");
      return false;
    }
    result.execution_mode = mode_str;
  } else {
    result.execution_mode = "sequential";
  }

  // 5. 解析 max_parallel_workers: 可选整数，范围 1~64
  if (root.contains("max_parallel_workers")) {
    // R1-ACC-003: sequential 模式禁止声明
    // max_parallel_workers，避免配置复制隐式错误
    if (result.execution_mode == "sequential") {
      SetDiag(
          diagnostic, PipelineErrorCode::kInvalidCombination,
          "/max_parallel_workers",
          "Field 'max_parallel_workers' is only allowed when execution_mode "
          "is 'parallel'");
      return false;
    }
    if (!root["max_parallel_workers"].is_number_integer()) {
      SetDiag(diagnostic, PipelineErrorCode::kFieldType,
              "/max_parallel_workers",
              "Field 'max_parallel_workers' must be an integer");
      return false;
    }
    int64_t workers = root["max_parallel_workers"].get<int64_t>();
    if (workers < 1 || workers > 64) {
      SetDiag(diagnostic, PipelineErrorCode::kFieldRange,
              "/max_parallel_workers",
              "Field 'max_parallel_workers' must be between 1 and 64");
      return false;
    }
    result.max_parallel_workers = static_cast<size_t>(workers);
  } else {
    result.max_parallel_workers = 4;
  }

  // 6. 解析 models: 可选数组，最多 64 个模型定义
  if (root.contains("models")) {
    if (!root["models"].is_array()) {
      SetDiag(diagnostic, PipelineErrorCode::kFieldType, "/models",
              "Field 'models' must be an array");
      return false;
    }
    if (root["models"].size() > 64) {
      SetDiag(diagnostic, PipelineErrorCode::kFieldRange, "/models",
              "Model count exceeds maximum limit of 64");
      return false;
    }

    const std::unordered_set<std::string> allowed_model_keys = {
        "model_id", "engine_type", "model_path", "config", "comment"};
    std::unordered_set<std::string> seen_model_ids;

    for (size_t i = 0; i < root["models"].size(); ++i) {
      const auto& model_elem = root["models"][i];
      std::string model_path_prefix = "/models/" + std::to_string(i);

      if (!model_elem.is_object()) {
        SetDiag(diagnostic, PipelineErrorCode::kFieldType, model_path_prefix,
                "Model item must be an object");
        return false;
      }

      // 拒绝 model 内部未知字段
      for (auto it = model_elem.begin(); it != model_elem.end(); ++it) {
        if (allowed_model_keys.find(it.key()) == allowed_model_keys.end()) {
          SetDiag(diagnostic, PipelineErrorCode::kUnknownField,
                  model_path_prefix + "/" + it.key(),
                  "Unknown field in model: " + it.key());
          return false;
        }
      }

      // comment 字段类型检查 (R1-ACC-006)
      if (model_elem.contains("comment") &&
          !model_elem["comment"].is_string()) {
        SetDiag(diagnostic, PipelineErrorCode::kFieldType,
                model_path_prefix + "/comment",
                "Field 'comment' must be a string");
        return false;
      }

      ParsedModelConfig model_cfg;
      model_cfg.source_index = i;

      // model_id (必填非空字符串，唯一)
      if (!model_elem.contains("model_id")) {
        SetDiag(diagnostic, PipelineErrorCode::kMissingField,
                model_path_prefix + "/model_id",
                "Missing required field 'model_id'");
        return false;
      }
      if (!model_elem["model_id"].is_string()) {
        SetDiag(diagnostic, PipelineErrorCode::kFieldType,
                model_path_prefix + "/model_id",
                "Field 'model_id' must be a string");
        return false;
      }
      model_cfg.model_id = model_elem["model_id"].get<std::string>();
      if (model_cfg.model_id.empty()) {
        SetDiag(diagnostic, PipelineErrorCode::kFieldRange,
                model_path_prefix + "/model_id",
                "Field 'model_id' cannot be empty");
        return false;
      }
      if (seen_model_ids.find(model_cfg.model_id) != seen_model_ids.end()) {
        SetDiag(diagnostic, PipelineErrorCode::kDuplicateModelId,
                model_path_prefix + "/model_id",
                "Duplicate model_id: " + model_cfg.model_id);
        return false;
      }
      seen_model_ids.insert(model_cfg.model_id);

      // engine_type (必填非空字符串)
      if (!model_elem.contains("engine_type")) {
        SetDiag(diagnostic, PipelineErrorCode::kMissingField,
                model_path_prefix + "/engine_type",
                "Missing required field 'engine_type'");
        return false;
      }
      if (!model_elem["engine_type"].is_string()) {
        SetDiag(diagnostic, PipelineErrorCode::kFieldType,
                model_path_prefix + "/engine_type",
                "Field 'engine_type' must be a string");
        return false;
      }
      model_cfg.engine_type = model_elem["engine_type"].get<std::string>();
      if (model_cfg.engine_type.empty()) {
        SetDiag(diagnostic, PipelineErrorCode::kFieldRange,
                model_path_prefix + "/engine_type",
                "Field 'engine_type' cannot be empty");
        return false;
      }

      // model_path (可选字符串)
      if (model_elem.contains("model_path")) {
        if (!model_elem["model_path"].is_string()) {
          SetDiag(diagnostic, PipelineErrorCode::kFieldType,
                  model_path_prefix + "/model_path",
                  "Field 'model_path' must be a string");
          return false;
        }
        model_cfg.model_path = model_elem["model_path"].get<std::string>();
      }

      // config (可选对象)
      if (model_elem.contains("config")) {
        if (!model_elem["config"].is_object()) {
          SetDiag(diagnostic, PipelineErrorCode::kFieldType,
                  model_path_prefix + "/config",
                  "Field 'config' must be an object");
          return false;
        }
        model_cfg.config = model_elem["config"];
      } else {
        model_cfg.config = nlohmann::json::object();
      }

      result.models.push_back(std::move(model_cfg));
    }
  }

  // 7. 解析 pipeline: 必填非空数组，最多 256 个节点定义
  if (!root.contains("pipeline")) {
    SetDiag(diagnostic, PipelineErrorCode::kMissingField, "/pipeline",
            "Missing required field 'pipeline'");
    return false;
  }
  if (!root["pipeline"].is_array()) {
    SetDiag(diagnostic, PipelineErrorCode::kFieldType, "/pipeline",
            "Field 'pipeline' must be an array");
    return false;
  }
  if (root["pipeline"].empty()) {
    SetDiag(diagnostic, PipelineErrorCode::kFieldRange, "/pipeline",
            "Pipeline cannot be empty");
    return false;
  }
  if (root["pipeline"].size() > 256) {
    SetDiag(diagnostic, PipelineErrorCode::kFieldRange, "/pipeline",
            "Pipeline node count exceeds maximum limit of 256");
    return false;
  }

  const std::unordered_set<std::string> allowed_node_keys = {
      "id", "node_type", "depends_on", "config", "comment"};

  std::unordered_set<std::string> seen_node_ids;

  for (size_t i = 0; i < root["pipeline"].size(); ++i) {
    const auto& node_elem = root["pipeline"][i];
    std::string node_path_prefix = "/pipeline/" + std::to_string(i);

    if (!node_elem.is_object()) {
      SetDiag(diagnostic, PipelineErrorCode::kFieldType, node_path_prefix,
              "Node item must be an object");
      return false;
    }

    // 拒绝 node 内部未知字段
    for (auto it = node_elem.begin(); it != node_elem.end(); ++it) {
      if (allowed_node_keys.find(it.key()) == allowed_node_keys.end()) {
        SetDiag(diagnostic, PipelineErrorCode::kUnknownField,
                node_path_prefix + "/" + it.key(),
                "Unknown field in node: " + it.key());
        return false;
      }
    }

    // comment 字段类型检查 (R1-ACC-006)
    if (node_elem.contains("comment") && !node_elem["comment"].is_string()) {
      SetDiag(diagnostic, PipelineErrorCode::kFieldType,
              node_path_prefix + "/comment",
              "Field 'comment' must be a string");
      return false;
    }

    ParsedNodeConfig node_cfg;
    node_cfg.source_index = i;

    // node_type (必填非空字符串)
    if (!node_elem.contains("node_type")) {
      SetDiag(diagnostic, PipelineErrorCode::kMissingField,
              node_path_prefix + "/node_type",
              "Missing required field 'node_type'");
      return false;
    }
    if (!node_elem["node_type"].is_string()) {
      SetDiag(diagnostic, PipelineErrorCode::kFieldType,
              node_path_prefix + "/node_type",
              "Field 'node_type' must be a string");
      return false;
    }
    node_cfg.node_type = node_elem["node_type"].get<std::string>();
    if (node_cfg.node_type.empty()) {
      SetDiag(diagnostic, PipelineErrorCode::kFieldRange,
              node_path_prefix + "/node_type",
              "Field 'node_type' cannot be empty");
      return false;
    }

    // config (可选对象)
    if (node_elem.contains("config")) {
      if (!node_elem["config"].is_object()) {
        SetDiag(diagnostic, PipelineErrorCode::kFieldType,
                node_path_prefix + "/config",
                "Field 'config' must be an object");
        return false;
      }
      node_cfg.config = node_elem["config"];
    } else {
      node_cfg.config = nlohmann::json::object();
    }

    // id (必填非空字符串，唯一)
    if (!node_elem.contains("id")) {
      SetDiag(diagnostic, PipelineErrorCode::kMissingField,
              node_path_prefix + "/id",
              "Missing required field 'id' in pipeline node");
      return false;
    }
    if (!node_elem["id"].is_string()) {
      SetDiag(diagnostic, PipelineErrorCode::kFieldType,
              node_path_prefix + "/id", "Field 'id' must be a string");
      return false;
    }
    node_cfg.id = node_elem["id"].get<std::string>();
    if (node_cfg.id.empty()) {
      SetDiag(diagnostic, PipelineErrorCode::kFieldRange,
              node_path_prefix + "/id", "Field 'id' cannot be empty");
      return false;
    }
    if (seen_node_ids.find(node_cfg.id) != seen_node_ids.end()) {
      SetDiag(diagnostic, PipelineErrorCode::kDuplicateNodeId,
              node_path_prefix + "/id", "Duplicate node id: " + node_cfg.id);
      return false;
    }
    seen_node_ids.insert(node_cfg.id);

    // depends_on (必填数组，元素为非空字符串且不重复)
    if (!node_elem.contains("depends_on")) {
      SetDiag(diagnostic, PipelineErrorCode::kMissingField,
              node_path_prefix + "/depends_on",
              "Missing required field 'depends_on' in pipeline node");
      return false;
    }
    if (!node_elem["depends_on"].is_array()) {
      SetDiag(diagnostic, PipelineErrorCode::kFieldType,
              node_path_prefix + "/depends_on",
              "Field 'depends_on' must be an array");
      return false;
    }
    if (node_elem["depends_on"].size() > 256) {
      SetDiag(diagnostic, PipelineErrorCode::kFieldRange,
              node_path_prefix + "/depends_on",
              "Node dependencies exceed limit of 256");
      return false;
    }

    std::unordered_set<std::string> node_deps;
    for (size_t d = 0; d < node_elem["depends_on"].size(); ++d) {
      const auto& dep_item = node_elem["depends_on"][d];
      std::string dep_path =
          node_path_prefix + "/depends_on/" + std::to_string(d);

      if (!dep_item.is_string()) {
        SetDiag(diagnostic, PipelineErrorCode::kFieldType, dep_path,
                "Dependency item must be a string");
        return false;
      }
      std::string dep_str = dep_item.get<std::string>();
      if (dep_str.empty()) {
        SetDiag(diagnostic, PipelineErrorCode::kFieldRange, dep_path,
                "Dependency item cannot be empty");
        return false;
      }
      if (node_deps.find(dep_str) != node_deps.end()) {
        SetDiag(diagnostic, PipelineErrorCode::kInvalidDependency, dep_path,
                "Duplicate dependency in node: " + dep_str);
        return false;
      }
      node_deps.insert(dep_str);
      node_cfg.depends_on.push_back(dep_str);
    }

    result.nodes.push_back(std::move(node_cfg));
  }

  *output = std::move(result);
  return true;
}

}  // namespace alg_framework

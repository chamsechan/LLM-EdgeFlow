#include "adapter/pipeline_document.h"

namespace llm_edgeflow {

bool SplitPipelineDocument(const nlohmann::json& root,
                           PipelineDocumentSplit* out_split,
                           std::string* out_error) {
  if (!out_split) {
    if (out_error) *out_error = "Null out_split pointer";
    return false;
  }
  *out_split = PipelineDocumentSplit{};

  if (!root.is_object()) {
    if (out_error) {
      *out_error = "Pipeline configuration root must be a JSON object";
    }
    return false;
  }

  if (!root.contains("deployment")) {
    out_split->has_deployment = false;
    out_split->neutral_pipeline_json = root;
    return true;
  }

  const auto& dep = root["deployment"];
  if (!dep.is_object()) {
    if (out_error) *out_error = "Field '/deployment' must be an object";
    return false;
  }

  // 1. 校验 deployment 内部白名单: 仅允许 model_paths 与 io
  for (auto it = dep.begin(); it != dep.end(); ++it) {
    if (it.key() != "model_paths" && it.key() != "io") {
      if (out_error) {
        *out_error = "Unknown field at /deployment/" + it.key() +
                     " (only 'model_paths' and 'io' allowed)";
      }
      return false;
    }
  }

  // 2. 校验 model_paths (可选)
  if (dep.contains("model_paths")) {
    const auto& mp = dep["model_paths"];
    if (!mp.is_object()) {
      if (out_error) {
        *out_error = "Field '/deployment/model_paths' must be an object";
      }
      return false;
    }
    for (auto it = mp.begin(); it != mp.end(); ++it) {
      if (it.key().empty()) {
        if (out_error) *out_error = "Empty model_id in /deployment/model_paths";
        return false;
      }
      if (!it.value().is_string()) {
        if (out_error) {
          *out_error = "Field '/deployment/model_paths/" + it.key() +
                       "' value must be a string";
        }
        return false;
      }
      const std::string path_str = it.value().get<std::string>();
      if (path_str.empty()) {
        if (out_error) {
          *out_error = "Field '/deployment/model_paths/" + it.key() +
                       "' cannot be empty";
        }
        return false;
      }
      out_split->deployment.model_paths[it.key()] = path_str;
    }
    out_split->deployment.has_model_paths = true;
  }

  // 3. 校验 io (当 deployment 存在时必填)
  if (!dep.contains("io")) {
    if (out_error) *out_error = "Missing required field '/deployment/io'";
    return false;
  }
  const auto& io = dep["io"];
  if (!io.is_object()) {
    if (out_error) *out_error = "Field '/deployment/io' must be an object";
    return false;
  }

  // 校验 io 内部白名单: 必须有且仅有 io_binding 与 output_allocations
  for (auto it = io.begin(); it != io.end(); ++it) {
    if (it.key() != "io_binding" && it.key() != "output_allocations") {
      if (out_error) {
        *out_error = "Unknown field at /deployment/io/" + it.key() +
                     " (only 'io_binding' and 'output_allocations' allowed)";
      }
      return false;
    }
  }

  if (!io.contains("io_binding")) {
    if (out_error) {
      *out_error = "Missing required field '/deployment/io/io_binding'";
    }
    return false;
  }
  if (!io["io_binding"].is_string()) {
    if (out_error) {
      *out_error = "Field '/deployment/io/io_binding' must be a string";
    }
    return false;
  }
  std::string binding_id = io["io_binding"].get<std::string>();
  if (binding_id.empty()) {
    if (out_error) {
      *out_error = "Field '/deployment/io/io_binding' cannot be empty";
    }
    return false;
  }
  out_split->deployment.io.io_binding = std::move(binding_id);

  if (!io.contains("output_allocations")) {
    if (out_error) {
      *out_error = "Missing required field '/deployment/io/output_allocations'";
    }
    return false;
  }
  if (!io["output_allocations"].is_object()) {
    if (out_error) {
      *out_error = "Field '/deployment/io/output_allocations' must be an object";
    }
    return false;
  }
  out_split->deployment.io.output_allocations = io["output_allocations"];
  out_split->deployment.has_io = true;

  out_split->has_deployment = true;
  out_split->neutral_pipeline_json = root;
  out_split->neutral_pipeline_json.erase("deployment");
  return true;
}

}  // namespace llm_edgeflow

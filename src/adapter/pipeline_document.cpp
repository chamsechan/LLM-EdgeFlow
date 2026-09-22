#include "adapter/pipeline_document.h"

#include "adapter/deployment_structure.h"
#include "contracts/json_structure.h"

namespace llm_edgeflow {

namespace structure = json_structure;

bool SplitPipelineDocument(const nlohmann::json& root,
                           PipelineDocumentSplit* out_split,
                           std::string* out_error,
                           std::string* out_error_path) {
  if (!out_split) {
    if (out_error) *out_error = "Null out_split pointer";
    return false;
  }
  *out_split = PipelineDocumentSplit{};

  if (!root.is_object()) {
    if (out_error) {
      *out_error = "Pipeline configuration root must be a JSON object";
    }
    if (out_error_path) *out_error_path = "/";
    return false;
  }

  if (!root.contains("deployment")) {
    out_split->has_deployment = false;
    out_split->neutral_pipeline_json = root;
    return true;
  }

  const auto& dep_shape = DeploymentStructure();
  const auto& dep = root["deployment"];
  if (!structure::HasType(dep, dep_shape)) {
    if (out_error) *out_error = "Field '/deployment' must be an object";
    if (out_error_path) *out_error_path = "/deployment";
    return false;
  }

  // 1. 校验 deployment 内部白名单: 仅允许 model_paths 与 io
  for (auto it = dep.begin(); it != dep.end(); ++it) {
    if (!structure::AllowsProperty(dep_shape, it.key())) {
      if (out_error) {
        *out_error = "Unknown field at /deployment/" +
                     EscapeJsonPointer(it.key()) +
                     " (only 'model_paths' and 'io' allowed)";
      }
      if (out_error_path) {
        *out_error_path = "/deployment/" + EscapeJsonPointer(it.key());
      }
      return false;
    }
  }

  // 2. 校验 model_paths (可选)
  if (dep.contains("model_paths")) {
    const auto& mp_shape = structure::Property(dep_shape, "model_paths");
    const auto& mp = dep["model_paths"];
    if (!structure::HasType(mp, mp_shape)) {
      if (out_error) {
        *out_error = "Field '/deployment/model_paths' must be an object";
      }
      if (out_error_path) *out_error_path = "/deployment/model_paths";
      return false;
    }
    for (auto it = mp.begin(); it != mp.end(); ++it) {
      if (structure::TooShort(it.key(), mp_shape.at("propertyNames"))) {
        if (out_error) *out_error = "Empty model_id in /deployment/model_paths";
        if (out_error_path) *out_error_path = "/deployment/model_paths";
        return false;
      }
      if (!structure::HasType(it.value(),
                              mp_shape.at("additionalProperties"))) {
        if (out_error) {
          *out_error = "Field '/deployment/model_paths/" +
                       EscapeJsonPointer(it.key()) + "' value must be a string";
        }
        if (out_error_path) {
          *out_error_path =
              "/deployment/model_paths/" + EscapeJsonPointer(it.key());
        }
        return false;
      }
      const std::string path_str = it.value().get<std::string>();
      if (structure::TooShort(it.value(),
                              mp_shape.at("additionalProperties"))) {
        if (out_error) {
          *out_error = "Field '/deployment/model_paths/" +
                       EscapeJsonPointer(it.key()) + "' cannot be empty";
        }
        if (out_error_path) {
          *out_error_path =
              "/deployment/model_paths/" + EscapeJsonPointer(it.key());
        }
        return false;
      }
      out_split->deployment.model_paths[it.key()] = path_str;
    }
    out_split->deployment.has_model_paths = true;
  }

  // 3. 校验 io (当 deployment 存在时必填)
  if (structure::MissingRequired(dep, dep_shape, "io")) {
    if (out_error) *out_error = "Missing required field '/deployment/io'";
    if (out_error_path) *out_error_path = "/deployment/io";
    return false;
  }
  const auto& io_shape = structure::Property(dep_shape, "io");
  const auto& io = dep["io"];
  if (!structure::HasType(io, io_shape)) {
    if (out_error) *out_error = "Field '/deployment/io' must be an object";
    if (out_error_path) *out_error_path = "/deployment/io";
    return false;
  }

  // 校验 io 内部白名单: 必须有且仅有 io_binding 与 output_allocations
  for (auto it = io.begin(); it != io.end(); ++it) {
    if (!structure::AllowsProperty(io_shape, it.key())) {
      if (out_error) {
        *out_error = "Unknown field at /deployment/io/" +
                     EscapeJsonPointer(it.key()) +
                     " (only 'io_binding' and 'output_allocations' allowed)";
      }
      if (out_error_path) {
        *out_error_path = "/deployment/io/" + EscapeJsonPointer(it.key());
      }
      return false;
    }
  }

  if (structure::MissingRequired(io, io_shape, "io_binding")) {
    if (out_error) {
      *out_error = "Missing required field '/deployment/io/io_binding'";
    }
    if (out_error_path) *out_error_path = "/deployment/io/io_binding";
    return false;
  }
  if (!structure::HasType(io["io_binding"],
                          structure::Property(io_shape, "io_binding"))) {
    if (out_error) {
      *out_error = "Field '/deployment/io/io_binding' must be a string";
    }
    if (out_error_path) *out_error_path = "/deployment/io/io_binding";
    return false;
  }
  std::string binding_id = io["io_binding"].get<std::string>();
  if (structure::TooShort(io["io_binding"],
                          structure::Property(io_shape, "io_binding"))) {
    if (out_error) {
      *out_error = "Field '/deployment/io/io_binding' cannot be empty";
    }
    if (out_error_path) *out_error_path = "/deployment/io/io_binding";
    return false;
  }
  out_split->deployment.io.io_binding = std::move(binding_id);

  if (structure::MissingRequired(io, io_shape, "output_allocations")) {
    if (out_error) {
      *out_error = "Missing required field '/deployment/io/output_allocations'";
    }
    if (out_error_path) *out_error_path = "/deployment/io/output_allocations";
    return false;
  }
  if (!structure::HasType(
          io["output_allocations"],
          structure::Property(io_shape, "output_allocations"))) {
    if (out_error) {
      *out_error =
          "Field '/deployment/io/output_allocations' must be an object";
    }
    if (out_error_path) *out_error_path = "/deployment/io/output_allocations";
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

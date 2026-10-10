#include "adapter/model_file_resolver.h"

#include "contracts/diagnostic.h"
#include "contracts/json_pointer.h"
#include "contracts/path_utils.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {
bool ResolveModelFiles(const nlohmann::json& pipeline_json,
                       const std::string& pipeline_dir,
                       nlohmann::json* resolved_pipeline_json,
                       std::string* error, DeploymentDiagnostic* diagnostic) {
  const auto fail = [&](const std::string& code, const std::string& path,
                        const std::string& message) {
    SetDiagnosticNoexcept(error, message);
    if (diagnostic) {
      diagnostic->code = code;
      diagnostic->path = path;
      diagnostic->message = message;
    }
    return false;
  };
  try {
    if (error) error->clear();
    if (diagnostic) diagnostic->Clear();
    if (!resolved_pipeline_json)
      return fail("DEPLOYMENT_ERROR", "/", "File resolver output is null");
    *resolved_pipeline_json = nlohmann::json();
    auto resolved = pipeline_json;
    if (!resolved.is_object() || !resolved.contains("models") ||
        !resolved["models"].is_array()) {
      *resolved_pipeline_json = std::move(resolved);
      return true;
    }
    const auto resolve = [&](nlohmann::json& value, const std::string& path) {
      // Core reports structural type errors; Integration owns filename rules.
      if (!value.is_string()) return true;
      std::filesystem::path file;
      std::string message;
      if (!ResolveFileUnderDirectory(pipeline_dir, value.get<std::string>(),
                                     &file, &message))
        return fail("INVALID_FILE_PATH", path, message);
      value = file.string();
      return true;
    };
    for (size_t i = 0; i < resolved["models"].size(); ++i) {
      auto& model = resolved["models"][i];
      const auto at = "/models/" + std::to_string(i);
      if (!model.is_object()) continue;
      if (!model.contains("type") || !model["type"].is_string() ||
          !model.contains("name") || !model["name"].is_string() ||
          model["name"].get_ref<const std::string&>().empty() ||
          !model.contains("file") || !model["file"].is_string() ||
          (model.contains("params") && !model["params"].is_object()) ||
          !model.contains("backend") || !model["backend"].is_object() ||
          !model["backend"].contains("type") ||
          !model["backend"]["type"].is_string() ||
          (model["backend"].contains("params") &&
           !model["backend"]["params"].is_object()))
        continue;
      const auto backend_type = model["backend"]["type"].get<std::string>();
      const auto implementations = ModelRegistry::Instance().FindImplementation(
          model["type"].get<std::string>(), backend_type);
      if (implementations.size() != 1) continue;
      const auto backend = BackendRegistry::Instance().Find(backend_type);
      if (!backend) continue;
      if (!resolve(model["file"], at + "/file")) return false;
      const auto resolve_params = [&](nlohmann::json& object,
                                      const ParameterSet& params,
                                      const std::string& path) {
        if (!object.contains("params") || !object["params"].is_object())
          return true;
        for (const auto& field : params.Fields())
          if (field.file && object["params"].contains(field.name) &&
              !resolve(object["params"][field.name],
                       path + "/params/" + EscapeJsonPointer(field.name)))
            return false;
        return true;
      };
      if (!resolve_params(model, implementations.front().params, at) ||
          !resolve_params(model["backend"], backend->params, at + "/backend"))
        return false;
    }
    *resolved_pipeline_json = std::move(resolved);
    return true;
  } catch (const std::exception& exception) {
    return fail("INTERNAL_EXCEPTION", "/", exception.what());
  } catch (...) {
    return fail("INTERNAL_EXCEPTION", "/", "Unknown file resolution exception");
  }
}
}  // namespace llm_edgeflow

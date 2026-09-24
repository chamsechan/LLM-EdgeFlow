#include "adapter/deployment_model_resolver.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "contracts/path_utils.h"

namespace llm_edgeflow {
namespace {

namespace fs = std::filesystem;

void SetDiagnostic(std::string* diagnostic,
                   DeploymentDiagnostic* out_diagnostic,
                   const std::string& code, const std::string& path,
                   const std::string& message) {
  if (diagnostic) *diagnostic = message;
  if (out_diagnostic) {
    out_diagnostic->code = code;
    out_diagnostic->path = path;
    out_diagnostic->message = message;

    out_diagnostic->pipeline_diagnostic.reset();
  }
}

}  // namespace

bool ResolveDeploymentModelPaths(
    const nlohmann::json& pipeline_json, const std::string& model_root_dir,
    nlohmann::json* resolved_pipeline_json, std::string* diagnostic,
    DeploymentDiagnostic* out_diagnostic) noexcept {
  try {
    if (out_diagnostic) out_diagnostic->Clear();
    if (!resolved_pipeline_json) {
      SetDiagnostic(diagnostic, out_diagnostic, "DEPLOYMENT_ERROR", "/",
                    "Deployment model resolver output is null");
      return false;
    }

    *resolved_pipeline_json = pipeline_json;

    if (!resolved_pipeline_json->is_object() ||
        !resolved_pipeline_json->contains("models") ||
        !(*resolved_pipeline_json)["models"].is_array()) {
      return true;
    }

    auto& models = (*resolved_pipeline_json)["models"];
    const bool has_model_path = std::any_of(
        models.begin(), models.end(), [](const nlohmann::json& model) {
          return model.is_object() && model.contains("model_path") &&
                 model["model_path"].is_string() &&
                 !model["model_path"].get_ref<const std::string&>().empty();
        });
    if (!has_model_path) return true;

    fs::path canonical_root;
    if (!model_root_dir.empty()) {
      std::error_code error;
      const fs::path absolute_root = fs::absolute(model_root_dir, error);
      if (error) {
        SetDiagnostic(
            diagnostic, out_diagnostic, "DEPLOYMENT_ERROR", "/",
            "Failed to make model_root_dir absolute: " + model_root_dir);
        return false;
      }
      canonical_root = fs::weakly_canonical(absolute_root, error);
      if (error || !fs::is_directory(canonical_root, error) || error) {
        SetDiagnostic(
            diagnostic, out_diagnostic, "DEPLOYMENT_ERROR", "/",
            "model_root_dir is not an accessible directory: " + model_root_dir);
        return false;
      }
    }

    for (size_t index = 0; index < models.size(); ++index) {
      auto& model = models[index];
      if (!model.is_object() || !model.contains("model_path") ||
          !model["model_path"].is_string() ||
          model["model_path"].get_ref<const std::string&>().empty()) {
        continue;
      }

      const std::string pointer =
          "/models/" + std::to_string(index) + "/model_path";
      const std::string raw_path = model["model_path"].get<std::string>();
      const fs::path normalized = fs::path(raw_path).lexically_normal();
      if (!normalized.is_absolute() && HasParentPathComponent(normalized)) {
        SetDiagnostic(diagnostic, out_diagnostic, "INVALID_MODEL_PATH", pointer,
                      "Model path cannot traverse outside model_root_dir at " +
                          pointer + ": " + raw_path);
        return false;
      }
      if (!normalized.is_absolute() && canonical_root.empty()) {
        SetDiagnostic(
            diagnostic, out_diagnostic, "INVALID_MODEL_PATH", pointer,
            "Relative model_path requires non-empty model_root_dir at " +
                pointer + ": " + raw_path);
        return false;
      }

      std::error_code error;
      const fs::path candidate = fs::weakly_canonical(
          normalized.is_absolute() ? normalized : canonical_root / normalized,
          error);
      if (error) {
        SetDiagnostic(diagnostic, out_diagnostic, "INVALID_MODEL_PATH", pointer,
                      "Failed to resolve deployment model path at " + pointer +
                          ": " + raw_path);
        return false;
      }
      if (!canonical_root.empty() &&
          !IsPathWithinRoot(canonical_root, candidate)) {
        SetDiagnostic(diagnostic, out_diagnostic, "INVALID_MODEL_PATH", pointer,
                      "Model path escapes model_root_dir at " + pointer + ": " +
                          raw_path);
        return false;
      }
      model["model_path"] = candidate.string();
    }
    return true;
  } catch (const std::exception& exception) {
    SetDiagnostic(
        diagnostic, out_diagnostic, "INTERNAL_EXCEPTION", "/",
        std::string("Deployment model path exception: ") + exception.what());
    return false;
  } catch (...) {
    SetDiagnostic(diagnostic, out_diagnostic, "INTERNAL_EXCEPTION", "/",
                  "Unknown deployment model path exception");
    return false;
  }
}

}  // namespace llm_edgeflow

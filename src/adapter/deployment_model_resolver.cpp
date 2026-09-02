#include "adapter/deployment_model_resolver.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace llm_edgeflow {
namespace {

namespace fs = std::filesystem;

bool IsWithinRoot(const fs::path& root, const fs::path& candidate) {
  auto root_it = root.begin();
  auto candidate_it = candidate.begin();
  while (root_it != root.end() && candidate_it != candidate.end() &&
         *root_it == *candidate_it) {
    ++root_it;
    ++candidate_it;
  }
  return root_it == root.end();
}

bool TraversesParent(const fs::path& path) {
  for (const auto& component : path) {
    if (component == "..") return true;
  }
  return false;
}

void SetDiagnostic(std::string* diagnostic, const std::string& message) {
  if (diagnostic) *diagnostic = message;
}

}  // namespace

bool ResolveDeploymentModelPaths(const nlohmann::json& pipeline_json,
                                 const std::string& model_root_dir,
                                 nlohmann::json* resolved_pipeline_json,
                                 std::string* diagnostic) noexcept {
  try {
    if (!resolved_pipeline_json) {
      SetDiagnostic(diagnostic, "Deployment model resolver output is null");
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
        SetDiagnostic(diagnostic, "Failed to make model_root_dir absolute: " +
                                      model_root_dir);
        return false;
      }
      canonical_root = fs::weakly_canonical(absolute_root, error);
      if (error || !fs::is_directory(canonical_root, error) || error) {
        SetDiagnostic(
            diagnostic,
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

      const std::string raw_path = model["model_path"].get<std::string>();
      const fs::path normalized = fs::path(raw_path).lexically_normal();
      if (!normalized.is_absolute() && TraversesParent(normalized)) {
        SetDiagnostic(diagnostic,
                      "Model path cannot traverse outside model_root_dir at "
                      "/models/" +
                          std::to_string(index) + "/model_path: " + raw_path);
        return false;
      }
      if (!normalized.is_absolute() && canonical_root.empty()) {
        SetDiagnostic(
            diagnostic,
            "Relative model_path requires non-empty model_root_dir at "
            "/models/" +
                std::to_string(index) + "/model_path: " + raw_path);
        return false;
      }

      std::error_code error;
      const fs::path candidate = fs::weakly_canonical(
          normalized.is_absolute() ? normalized : canonical_root / normalized,
          error);
      if (error) {
        SetDiagnostic(diagnostic,
                      "Failed to resolve deployment model path at /models/" +
                          std::to_string(index) + "/model_path: " + raw_path);
        return false;
      }
      if (!canonical_root.empty() && !IsWithinRoot(canonical_root, candidate)) {
        SetDiagnostic(diagnostic,
                      "Model path escapes model_root_dir at /models/" +
                          std::to_string(index) + "/model_path: " + raw_path);
        return false;
      }
      model["model_path"] = candidate.string();
    }
    return true;
  } catch (const std::exception& exception) {
    SetDiagnostic(diagnostic, std::string("Deployment model path exception: ") +
                                  exception.what());
    return false;
  } catch (...) {
    SetDiagnostic(diagnostic, "Unknown deployment model path exception");
    return false;
  }
}

}  // namespace llm_edgeflow

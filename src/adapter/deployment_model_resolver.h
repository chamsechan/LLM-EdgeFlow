#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>

namespace llm_edgeflow {

/**
 * @brief Resolve deployment model references before entering Orchestration.
 *
 * A non-empty model_root_dir denotes the directory that directly contains
 * model artifacts and sidecars. Relative model_path values are resolved under
 * that directory and cannot escape it. With an empty root, deployment model
 * paths must already be absolute.
 */
bool ResolveDeploymentModelPaths(
    const nlohmann::json& pipeline_json, const std::string& model_root_dir,
    nlohmann::json* resolved_pipeline_json, std::string* diagnostic,
    const std::unordered_set<std::string>& overridden_model_ids = {}) noexcept;

}  // namespace llm_edgeflow

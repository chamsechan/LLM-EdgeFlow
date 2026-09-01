#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace alg_framework {

/**
 * @brief Resolve deployment model references before entering Layer 2.
 *
 * A non-empty model_root_dir denotes the directory that directly contains
 * model artifacts and sidecars. Relative model_path values are resolved under
 * that directory and cannot escape it. With an empty root, deployment model
 * paths must already be absolute.
 */
bool ResolveDeploymentModelPaths(const nlohmann::json& pipeline_json,
                                 const std::string& model_root_dir,
                                 nlohmann::json* resolved_pipeline_json,
                                 std::string* diagnostic) noexcept;

}  // namespace alg_framework

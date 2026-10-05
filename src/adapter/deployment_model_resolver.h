#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "adapter/deployment_diagnostic.h"

namespace llm_edgeflow {

/**
 * @brief 进入流程编排层前解析部署中的模型引用。
 *
 * 非空的 model_root_dir 表示直接存放模型文件及其附属文件的目录。相对的
 * model_path 在该目录下解析，且不能越出该目录。根目录为空时，部署中的
 * 模型路径必须已是绝对路径。
 */
bool ResolveDeploymentModelPaths(
    const nlohmann::json& pipeline_json, const std::string& model_root_dir,
    nlohmann::json* resolved_pipeline_json, std::string* diagnostic,
    DeploymentDiagnostic* out_diagnostic = nullptr) noexcept;

}  // namespace llm_edgeflow

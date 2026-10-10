#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "adapter/deployment_diagnostic.h"

namespace llm_edgeflow {
// file 与声明为文件的参数均相对 Pipeline JSON 的目录；目录未知时只校验写法。
bool ResolveModelFiles(const nlohmann::json& pipeline_json,
                       const std::string& pipeline_dir,
                       nlohmann::json* resolved_pipeline_json,
                       std::string* error,
                       DeploymentDiagnostic* diagnostic = nullptr);
}  // namespace llm_edgeflow

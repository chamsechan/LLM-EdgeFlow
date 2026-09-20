#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "adapter/deployment_diagnostic.h"

namespace llm_edgeflow {

/**
 * @brief 部署配置文件解析结构 (RFC-0061: 仅包含启动定位字段 pipe_path)
 */
struct DeploymentIoConfig {
  std::string pipe_path;
  std::string resolved_pipe_path;

  static bool Parse(const nlohmann::json& root, const std::string& config_dir,
                    DeploymentIoConfig* out_config, std::string* out_error,
                    DeploymentDiagnostic* out_diagnostic = nullptr);

  static bool ReadFromFile(const std::string& config_path,
                           DeploymentIoConfig* out_config,
                           std::string* out_error,
                           DeploymentDiagnostic* out_diagnostic = nullptr);
};

}  // namespace llm_edgeflow

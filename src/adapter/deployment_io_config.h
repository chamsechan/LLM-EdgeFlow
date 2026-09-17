#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace llm_edgeflow {

/**
 * @brief 部署配置文件解析结构 (RFC-0061: 仅包含启动定位字段 pipe_path)
 */
struct DeploymentIoConfig {
  std::string pipe_path;
  std::string resolved_pipe_path;
  nlohmann::json raw_json;

  static bool Parse(const nlohmann::json& root, const std::string& config_dir,
                    const std::string& transport,
                    DeploymentIoConfig* out_config, std::string* out_error);

  static bool ReadFromFile(const std::string& config_path,
                           const std::string& transport,
                           DeploymentIoConfig* out_config,
                           std::string* out_error);
};

}  // namespace llm_edgeflow

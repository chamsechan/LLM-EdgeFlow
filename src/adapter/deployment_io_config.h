#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

namespace llm_edgeflow {

/**
 * @brief Schema 1 统一接入配置文件解析结构
 */
struct DeploymentIoConfig {
  int schema_version = 1;
  std::string pipe_path;
  std::string io_binding;
  std::unordered_map<std::string, std::string> model_paths;
  nlohmann::json outputs = nlohmann::json::object();
  std::string resolved_pipe_path;
  nlohmann::json raw_json;

  static bool Parse(const nlohmann::json& root, const std::string& config_dir,
                    const std::string& transport,
                    DeploymentIoConfig* out_config, std::string* out_error,
                    const std::string& root_dir = "");

  static bool ReadFromFile(const std::string& config_path,
                           const std::string& transport,
                           DeploymentIoConfig* out_config,
                           std::string* out_error,
                           const std::string& root_dir = "");
};

}  // namespace llm_edgeflow

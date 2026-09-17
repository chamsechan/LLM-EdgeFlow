#include "adapter/deployment_io_config.h"

#include <filesystem>
#include <fstream>

#include "contracts/path_utils.h"

namespace llm_edgeflow {

namespace fs = std::filesystem;

bool DeploymentIoConfig::ReadFromFile(const std::string& config_path,
                                      const std::string& transport,
                                      DeploymentIoConfig* out_config,
                                      std::string* out_error) {
  if (config_path.empty()) {
    if (out_error) *out_error = "Empty config_path";
    return false;
  }

  std::ifstream ifs(config_path);
  if (!ifs.is_open()) {
    if (out_error) *out_error = "Failed to open config file: " + config_path;
    return false;
  }

  nlohmann::json root;
  try {
    ifs >> root;
  } catch (const std::exception& e) {
    if (out_error) {
      *out_error = "JSON parse exception in " + config_path + ": " + e.what();
    }
    return false;
  }

  fs::path cfg_dir = fs::path(config_path).parent_path();
  if (cfg_dir.empty()) {
    cfg_dir = ".";
  }
  cfg_dir = fs::absolute(cfg_dir);

  return Parse(root, cfg_dir.string(), transport, out_config, out_error);
}

bool DeploymentIoConfig::Parse(const nlohmann::json& root,
                               const std::string& config_dir,
                               const std::string& transport,
                               DeploymentIoConfig* out_config,
                               std::string* out_error) {
  if (!out_config) {
    if (out_error) *out_error = "Null out_config pointer";
    return false;
  }

  if (!root.is_object()) {
    if (out_error) *out_error = "Root configuration must be a JSON object";
    return false;
  }

  if (transport != "operator") {
    if (out_error) {
      *out_error = "Unsupported transport: '" + transport +
                   "' (only 'operator' is supported)";
    }
    return false;
  }

  // 1. 检查并明确拒绝旧 Schema 1 字段及外部分散配置 (RFC-0061)
  if (root.contains("schema_version") || root.contains("data") ||
      root.contains("io_binding") || root.contains("model_paths") ||
      root.contains("outputs")) {
    if (out_error) {
      *out_error =
          "Deprecated deployment configuration format (RFC-0061): "
          "'.conf' files must contain only 'pipe_path'. Deployment configuration "
          "(io_binding, output_allocations, model_paths) has moved to the "
          "'deployment' section inside the Pipeline JSON.";
    }
    return false;
  }

  // 2. 根字段白名单: 必须有且仅有 pipe_path
  for (auto it = root.begin(); it != root.end(); ++it) {
    if (it.key() != "pipe_path") {
      if (out_error) {
        *out_error = "Unknown field at /: '" + it.key() +
                     "' (only 'pipe_path' is allowed under RFC-0061)";
      }
      return false;
    }
  }

  if (!root.contains("pipe_path") || !root["pipe_path"].is_string() ||
      root["pipe_path"].get<std::string>().empty()) {
    if (out_error) *out_error = "Missing or empty 'pipe_path'";
    return false;
  }

  out_config->pipe_path = root["pipe_path"].get<std::string>();
  out_config->raw_json = root;

  // 3. 解析 pipe_path 相对 config_dir，严格限制在配置根目录下，拒绝任何逃逸与搜索回退
  fs::path base_dir = fs::absolute(fs::path(config_dir));
  fs::path raw_pipe = fs::path(out_config->pipe_path);
  fs::path full_pipe =
      raw_pipe.is_absolute() ? raw_pipe : (base_dir / raw_pipe);

  std::error_code ec;
  fs::path canonical_base = fs::weakly_canonical(base_dir, ec);
  fs::path canonical_pipe = fs::weakly_canonical(full_pipe, ec);

  if (!IsPathWithinRoot(canonical_base, canonical_pipe)) {
    if (out_error) {
      *out_error =
          "pipe_path escapes config directory: " + out_config->pipe_path;
    }
    return false;
  }

  if (!fs::exists(canonical_pipe)) {
    if (out_error) {
      *out_error = "Pipeline file does not exist: " + canonical_pipe.string();
    }
    return false;
  }

  // 校验符号链接目标，防止符号链接逃出配置目录
  fs::path real_pipe = fs::canonical(canonical_pipe, ec);
  if (ec || !IsPathWithinRoot(canonical_base, real_pipe)) {
    if (out_error) {
      *out_error =
          "pipe_path escapes config directory: " + out_config->pipe_path;
    }
    return false;
  }

  out_config->resolved_pipe_path = real_pipe.string();
  return true;
}

}  // namespace llm_edgeflow

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

  // 1. 顶层字段白名单检查: 仅允许 schema_version 和 data
  for (auto it = root.begin(); it != root.end(); ++it) {
    if (it.key() != "schema_version" && it.key() != "data") {
      if (out_error) {
        *out_error = "Unknown field at /: " + it.key() +
                     " (only schema_version and data allowed)";
      }
      return false;
    }
  }

  if (!root.contains("schema_version")) {
    if (out_error) *out_error = "Missing schema_version in config";
    return false;
  }
  if (!root["schema_version"].is_number_integer()) {
    if (out_error) *out_error = "schema_version must be an integer";
    return false;
  }
  int ver = root["schema_version"].get<int>();
  if (ver != 1) {
    if (out_error) {
      *out_error =
          "Unsupported schema_version " + std::to_string(ver) + ", expected 1";
    }
    return false;
  }

  if (!root.contains("data") || !root["data"].is_object()) {
    if (out_error) *out_error = "Missing or invalid 'data' object in config";
    return false;
  }

  const auto& data = root["data"];

  // 2. data 内部字段检查
  for (auto it = data.begin(); it != data.end(); ++it) {
    if (it.key() != "pipe_path" && it.key() != "io_binding" &&
        it.key() != "model_paths" && it.key() != "outputs") {
      if (out_error) {
        *out_error = "Unknown field in conf data: '" + it.key() + "'";
      }
      return false;
    }
  }

  if (!data.contains("pipe_path") || !data["pipe_path"].is_string() ||
      data["pipe_path"].get<std::string>().empty()) {
    if (out_error) *out_error = "Missing or empty 'data.pipe_path'";
    return false;
  }

  if (!data.contains("io_binding") || !data["io_binding"].is_string() ||
      data["io_binding"].get<std::string>().empty()) {
    if (out_error) *out_error = "Missing or empty 'data.io_binding'";
    return false;
  }

  out_config->schema_version = ver;
  out_config->pipe_path = data["pipe_path"].get<std::string>();
  out_config->io_binding = data["io_binding"].get<std::string>();
  out_config->raw_json = root;

  // 3. outputs 约束
  if (data.contains("outputs")) {
    if (transport == "cabi") {
      if (out_error) {
        *out_error = "C ABI deployment config does not accept 'data.outputs'";
      }
      return false;
    }
    if (!data["outputs"].is_object()) {
      if (out_error) *out_error = "data.outputs must be an object";
      return false;
    }
    out_config->outputs = data["outputs"];
  } else {
    out_config->outputs = nlohmann::json::object();
  }

  // 4. model_paths
  out_config->model_paths.clear();
  if (data.contains("model_paths")) {
    if (!data["model_paths"].is_object()) {
      if (out_error) *out_error = "data.model_paths must be an object";
      return false;
    }
    for (auto it = data["model_paths"].begin(); it != data["model_paths"].end();
         ++it) {
      if (!it.value().is_string()) {
        if (out_error) {
          *out_error =
              "data.model_paths[" + it.key() + "] value must be a string";
        }
        return false;
      }
      out_config->model_paths[it.key()] = it.value().get<std::string>();
    }
  }

  // 5. 解析 pipe_path 相对
  // config_dir，严格限制在配置根目录下，拒绝任何逃逸与搜索回退
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
          "data.pipe_path escapes config directory: " + out_config->pipe_path;
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
          "data.pipe_path escapes config directory: " + out_config->pipe_path;
    }
    return false;
  }

  out_config->resolved_pipe_path = real_pipe.string();
  return true;
}

}  // namespace llm_edgeflow

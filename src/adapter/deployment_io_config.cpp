#include "adapter/deployment_io_config.h"

#include <filesystem>
#include <fstream>

#include "adapter/pipeline_document.h"
#include "contracts/path_utils.h"

namespace llm_edgeflow {

namespace fs = std::filesystem;

static void SetConfigDiag(DeploymentDiagnostic* out_diag,
                          const std::string& code, const std::string& path,
                          const std::string& message) {
  if (out_diag) {
    out_diag->code = code;
    out_diag->path = path;
    out_diag->message = message;
    out_diag->legacy_status = -2;
    out_diag->pipeline_diagnostic.reset();
  }
}

bool DeploymentIoConfig::ReadFromFile(const std::string& config_path,
                                      const std::string& transport,
                                      DeploymentIoConfig* out_config,
                                      std::string* out_error,
                                      DeploymentDiagnostic* out_diagnostic) {
  if (out_diagnostic) out_diagnostic->Clear();

  if (config_path.empty()) {
    if (out_error) *out_error = "Empty config_path";
    SetConfigDiag(out_diagnostic, "DEPLOYMENT_ERROR", "/", "Empty config_path");
    return false;
  }

  std::ifstream ifs(config_path);
  if (!ifs.is_open()) {
    std::string msg = "Failed to open config file: " + config_path;
    if (out_error) *out_error = msg;
    SetConfigDiag(out_diagnostic, "CONFIG_FILE_OPEN", "/", msg);
    return false;
  }

  nlohmann::json root;
  try {
    ifs >> root;
  } catch (const std::exception& e) {
    std::string msg =
        "JSON parse exception in " + config_path + ": " + e.what();
    if (out_error) *out_error = msg;
    SetConfigDiag(out_diagnostic, "JSON_PARSE", "/", msg);
    return false;
  }

  fs::path cfg_dir = fs::path(config_path).parent_path();
  if (cfg_dir.empty()) {
    cfg_dir = ".";
  }
  cfg_dir = fs::absolute(cfg_dir);

  std::string parse_err;
  DeploymentDiagnostic parse_diag;
  bool ok = Parse(root, cfg_dir.string(), transport, out_config, &parse_err,
                  &parse_diag);
  if (!ok) {
    std::string prefix = "Error in config file " + config_path + ": ";
    if (out_error) *out_error = prefix + parse_err;
    if (out_diagnostic) {
      *out_diagnostic = parse_diag;
      out_diagnostic->message = prefix + out_diagnostic->message;
    }
    return false;
  }
  if (out_diagnostic) *out_diagnostic = parse_diag;
  return true;
}

bool DeploymentIoConfig::Parse(const nlohmann::json& root,
                               const std::string& config_dir,
                               const std::string& transport,
                               DeploymentIoConfig* out_config,
                               std::string* out_error,
                               DeploymentDiagnostic* out_diagnostic) {
  if (out_diagnostic) out_diagnostic->Clear();

  if (!out_config) {
    if (out_error) *out_error = "Null out_config pointer";
    SetConfigDiag(out_diagnostic, "DEPLOYMENT_ERROR", "/",
                  "Null out_config pointer");
    return false;
  }

  if (!root.is_object()) {
    if (out_error) *out_error = "Root configuration must be a JSON object";
    SetConfigDiag(out_diagnostic, "DEPLOYMENT_ERROR", "/",
                  "Root configuration must be a JSON object");
    return false;
  }

  if (transport != "operator") {
    std::string msg = "Unsupported transport: '" + transport +
                      "' (only 'operator' is supported)";
    if (out_error) *out_error = msg;
    SetConfigDiag(out_diagnostic, "UNSUPPORTED_TRANSPORT", "/", msg);
    return false;
  }

  // 1. 检查并明确拒绝旧 Schema 1 字段及外部分散配置 (RFC-0061)
  for (const char* deprecated_key :
       {"data", "schema_version", "io_binding", "model_paths", "outputs"}) {
    if (root.contains(deprecated_key)) {
      std::string escaped_key = EscapeJsonPointer(deprecated_key);
      std::string msg =
          std::string(
              "Deprecated deployment configuration format (RFC-0061) at /") +
          escaped_key +
          ": '.conf' files must contain only 'pipe_path'. Deployment "
          "configuration "
          "(io_binding, output_allocations, model_paths) has moved to the "
          "'deployment' section inside the Pipeline JSON.";
      if (out_error) *out_error = msg;
      SetConfigDiag(out_diagnostic, "DEPLOYMENT_ERROR",
                    std::string("/") + escaped_key, msg);
      return false;
    }
  }

  // 2. 根字段白名单: 必须有且仅有 pipe_path
  for (auto it = root.begin(); it != root.end(); ++it) {
    if (it.key() != "pipe_path") {
      std::string escaped_key = EscapeJsonPointer(it.key());
      std::string msg = "Unknown field at /: '" + it.key() +
                        "' (only 'pipe_path' is allowed under RFC-0061)";
      if (out_error) *out_error = msg;
      SetConfigDiag(out_diagnostic, "DEPLOYMENT_ERROR", "/" + escaped_key, msg);
      return false;
    }
  }

  if (!root.contains("pipe_path") || !root["pipe_path"].is_string() ||
      root["pipe_path"].get<std::string>().empty()) {
    if (out_error) *out_error = "Missing or empty 'pipe_path'";
    SetConfigDiag(out_diagnostic, "DEPLOYMENT_ERROR", "/pipe_path",
                  "Missing or empty 'pipe_path'");
    return false;
  }

  out_config->pipe_path = root["pipe_path"].get<std::string>();
  out_config->raw_json = root;

  // 3. 解析 pipe_path 相对
  // config_dir，严格限制在配置根目录下，拒绝任何逃逸与搜索回退
  fs::path base_dir = fs::absolute(fs::path(config_dir));
  fs::path raw_pipe = fs::path(out_config->pipe_path);
  fs::path full_pipe =
      raw_pipe.is_absolute() ? raw_pipe : (base_dir / raw_pipe);

  std::error_code ec;
  fs::path canonical_base = fs::weakly_canonical(base_dir, ec);
  fs::path canonical_pipe = fs::weakly_canonical(full_pipe, ec);

  if (!IsPathWithinRoot(canonical_base, canonical_pipe)) {
    std::string msg =
        "pipe_path escapes config directory: " + out_config->pipe_path;
    if (out_error) *out_error = msg;
    SetConfigDiag(out_diagnostic, "DEPLOYMENT_ERROR", "/pipe_path", msg);
    return false;
  }

  if (!fs::exists(canonical_pipe)) {
    std::string msg =
        "Pipeline file does not exist: " + canonical_pipe.string();
    if (out_error) *out_error = msg;
    SetConfigDiag(out_diagnostic, "DEPLOYMENT_ERROR", "/pipe_path", msg);
    return false;
  }

  // 校验符号链接目标，防止符号链接逃出配置目录
  fs::path real_pipe = fs::canonical(canonical_pipe, ec);
  if (ec || !IsPathWithinRoot(canonical_base, real_pipe)) {
    std::string msg =
        "pipe_path escapes config directory: " + out_config->pipe_path;
    if (out_error) *out_error = msg;
    SetConfigDiag(out_diagnostic, "DEPLOYMENT_ERROR", "/pipe_path", msg);
    return false;
  }

  out_config->resolved_pipe_path = real_pipe.string();
  return true;
}

}  // namespace llm_edgeflow

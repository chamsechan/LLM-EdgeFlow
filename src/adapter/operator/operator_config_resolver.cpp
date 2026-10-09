#include "adapter/operator/operator_config_resolver.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "adapter/deployment_io_config.h"
#include "adapter/io_plan_resolver.h"
#include "adapter/pipeline_document.h"
#include "contracts/diagnostic.h"
#include "contracts/path_utils.h"

namespace llm_edgeflow {

namespace {

int ResolveRequiredFileUnderRoot(const std::filesystem::path& canonical_root,
                                 const std::string& relative_value,
                                 const char* field_name,
                                 std::filesystem::path* resolved,
                                 std::string* error_msg) noexcept {
  try {
    if (!resolved) return -2;
    if (relative_value.empty()) {
      if (error_msg) *error_msg = std::string(field_name) + " path is empty";
      return -2;
    }
    // 拒绝 POSIX / Windows / UNC 绝对路径与盘符
    if (relative_value[0] == '/' || relative_value[0] == '\\') {
      if (error_msg) {
        *error_msg = std::string(field_name) +
                     " must be relative, got absolute: " + relative_value;
      }
      return -2;
    }
    if (relative_value.size() >= 2 &&
        ((relative_value[0] >= 'a' && relative_value[0] <= 'z') ||
         (relative_value[0] >= 'A' && relative_value[0] <= 'Z')) &&
        relative_value[1] == ':') {
      if (error_msg) {
        *error_msg = std::string(field_name) +
                     " contains Windows drive letter: " + relative_value;
      }
      return -2;
    }

    std::filesystem::path rel_path(relative_value);
    if (rel_path.is_absolute() || rel_path.has_root_name() ||
        rel_path.has_root_directory()) {
      if (error_msg) {
        *error_msg =
            std::string(field_name) + " has absolute root: " + relative_value;
      }
      return -2;
    }

    std::error_code ec;
    std::filesystem::path combined =
        (canonical_root / rel_path).lexically_normal();
    if (!IsPathWithinRoot(canonical_root, combined)) {
      if (error_msg) {
        *error_msg =
            std::string(field_name) + " escapes model_path: " + relative_value;
      }
      return -2;
    }

    std::filesystem::path canon_p;

    if (!std::filesystem::exists(combined, ec) || ec) {
      if (error_msg) {
        *error_msg = std::string(field_name) +
                     " file does not exist: " + combined.string();
      }
      return -2;
    }
    canon_p = std::filesystem::canonical(combined, ec);
    if (ec) {
      if (error_msg) {
        *error_msg = "Failed to canonicalize " + std::string(field_name) +
                     ": " + combined.string();
      }
      return -2;
    }

    if (!IsPathWithinRoot(canonical_root, canon_p)) {
      if (error_msg) {
        *error_msg = std::string(field_name) +
                     " symlink escapes model_path: " + canon_p.string();
      }
      return -2;
    }

    if (!std::filesystem::is_regular_file(canon_p, ec) || ec) {
      if (error_msg)
        *error_msg = std::string(field_name) +
                     " must be a regular file: " + canon_p.string();
      return -2;
    }

    *resolved = canon_p;
    return 0;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(error_msg, e.what());
    return -2;
  } catch (...) {
    SetDiagnosticNoexcept(error_msg, "Unknown exception");
    return -2;
  }
}

}  // namespace

int OperatorConfigResolver::Resolve(
    const char* model_path, const char* cfg_file_name,
    ResolvedOperatorConfig* result, std::string* error_msg,
    uint32_t max_frame_depth, DeploymentDiagnostic* out_diagnostic) noexcept {
  auto set_diag = [&](const std::string& code, const std::string& path,
                      const std::string& message) {
    if (error_msg) *error_msg = message;
    if (out_diagnostic) {
      out_diagnostic->code = code;
      out_diagnostic->path = path;
      out_diagnostic->message = message;

      out_diagnostic->pipeline_diagnostic.reset();
    }
  };

  try {
    if (out_diagnostic) out_diagnostic->Clear();

    if (!result) {
      set_diag("DEPLOYMENT_ERROR", "/", "Null output result pointer");
      return -2;
    }

    if (!model_path || model_path[0] == '\0') {
      set_diag("DEPLOYMENT_ERROR", "/", "Null or empty model_path");
      return -2;
    }

    if (!cfg_file_name || cfg_file_name[0] == '\0') {
      set_diag("DEPLOYMENT_ERROR", "/", "Null or empty cfg_file_name");
      return -2;
    }

    uint32_t effective_depth =
        max_frame_depth > 0 ? max_frame_depth : kDefaultOutputPoolDepth;
    if (effective_depth > kMaxOutputPoolDepth) {
      std::string msg = "max_frame_depth (" + std::to_string(effective_depth) +
                        ") exceeds hard limit " +
                        std::to_string(kMaxOutputPoolDepth);
      set_diag("DEPLOYMENT_ERROR", "/", msg);
      return -2;
    }

    std::filesystem::path raw_root(model_path);
    std::error_code ec;
    if (!std::filesystem::exists(raw_root, ec) ||
        !std::filesystem::is_directory(raw_root, ec)) {
      std::string msg =
          "model_path directory does not exist: " + raw_root.string();
      set_diag("DEPLOYMENT_ERROR", "/", msg);
      return -2;
    }

    std::filesystem::path canon_root = std::filesystem::canonical(raw_root, ec);
    if (ec) {
      std::string msg =
          "Failed to canonicalize model_path: " + raw_root.string();
      set_diag("DEPLOYMENT_ERROR", "/", msg);
      return -2;
    }

    // 沙箱解析 cfg_file_name
    std::filesystem::path full_cfg;
    int ret = ResolveRequiredFileUnderRoot(
        canon_root, cfg_file_name, "cfg_file_name", &full_cfg, error_msg);
    if (ret != 0) {
      if (out_diagnostic) {
        out_diagnostic->code = "DEPLOYMENT_ERROR";
        out_diagnostic->path = "/";
        out_diagnostic->message =
            error_msg ? *error_msg : "Failed to resolve cfg_file_name";
      }
      return ret;
    }

    // 读取并解析部署配置文件
    DeploymentIoConfig dep_config;
    std::string dep_err;
    if (!DeploymentIoConfig::ReadFromFile(full_cfg.string(), &dep_config,
                                          &dep_err, out_diagnostic)) {
      if (error_msg) *error_msg = dep_err;
      return -2;
    }

    // 解析接入计划
    std::unique_ptr<ValidatedIoPlan> io_plan;
    std::string plan_err;
    int plan_ret = IoPlanResolver::ResolveFromConfig(
        dep_config, canon_root.string(), &io_plan, &plan_err, out_diagnostic,
        effective_depth);
    if (plan_ret != 0) {
      if (error_msg) *error_msg = plan_err;
      return plan_ret;
    }

    result->conf_path = full_cfg;
    result->pipeline_path = dep_config.resolved_pipe_path;
    result->model_root_path = canon_root;
    result->effective_frame_depth = effective_depth;
    result->effective_process_batch_limit = static_cast<uint32_t>(
        std::min<size_t>(effective_depth, kMaxProcessBatchSize));

    result->io_plan = std::move(io_plan);

    return 0;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(error_msg, e.what());
    if (out_diagnostic) {
      out_diagnostic->code = "INTERNAL_EXCEPTION";
      out_diagnostic->path = "/";
      out_diagnostic->message = e.what();
    }
    return -2;
  } catch (...) {
    SetDiagnosticNoexcept(error_msg, "Unknown exception");
    if (out_diagnostic) {
      out_diagnostic->code = "INTERNAL_EXCEPTION";
      out_diagnostic->path = "/";
      out_diagnostic->message = "Unknown exception";
    }
    return -2;
  }
}

}  // namespace llm_edgeflow

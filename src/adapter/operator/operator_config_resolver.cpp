#include "adapter/operator/operator_config_resolver.h"

#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "adapter/deployment_io_config.h"
#include "adapter/io_binding_resolver.h"
#include "adapter/operator/json_output_config_reader.h"
#include "contracts/diagnostic.h"
#include "contracts/path_utils.h"

namespace llm_edgeflow {

namespace {

int ResolveOutputAllocation(const nlohmann::json& config,
                            const ExternalSlotDefinition& slot,
                            ResolvedOutputPoolSpec* result,
                            std::string* parameter_text, std::string* error) {
  if (!config.is_object()) {
    if (error) *error = "Output allocation must be an object";
    return -2;
  }
  static const std::unordered_set<std::string> fields = {
      "type",     "allocator",        "params",
      "meta_num", "metadata_type_id", "capacities"};
  for (const auto& [field, value] : config.items()) {
    if (!fields.count(field)) {
      if (error) *error = "Unknown output allocation field: " + field;
      return -2;
    }
  }
  if (!config.contains("type") || !config["type"].is_string()) {
    if (error) *error = "Missing required 'type' string in output allocation";
    return -2;
  }
  ResolvedOutputPoolSpec requested;
  requested.type = config["type"].get<std::string>();
  if (requested.type != slot.type_suffix) {
    if (error)
      *error = "Output type '" + requested.type + "' does not match slot '" +
               slot.slot_name + "'";
    return -2;
  }
  if (config.contains("allocator")) {
    if (!config["allocator"].is_string() ||
        config["allocator"].get<std::string>().empty()) {
      if (error) *error = "Output allocator must be a nonempty string";
      return -2;
    }
    requested.allocator = config["allocator"].get<std::string>();
  }
  const auto* binding = OperatorValueTypeRegistry::Instance().GetOutputBinding(
      requested.type, requested.allocator);
  if (!binding) {
    if (error)
      *error = "No registered output allocator '" + requested.allocator +
               "' for type '" + requested.type + "'";
    return -2;
  }
  const JsonOutputConfigReader reader(config);
  if (!reader.Read(OutputConfigField::kParameters, parameter_text, error)) {
    return -2;
  }
  if (!NormalizeOutputParameters(*binding, *parameter_text, &requested.params,
                                 error)) {
    return -2;
  }
  if (config.contains("meta_num")) {
    if (!config["meta_num"].is_number_unsigned()) {
      if (error) *error = "config.meta_num must be non-negative integer";
      return -2;
    }
    uint64_t mnum = config["meta_num"].get<uint64_t>();
    if (mnum > std::numeric_limits<uint32_t>::max()) {
      if (error) *error = "config.meta_num exceeds uint32 range";
      return -2;
    }
    requested.meta_num = static_cast<uint32_t>(mnum);
  }

  if (config.contains("metadata_type_id")) {
    if (config["metadata_type_id"].is_number_unsigned()) {
      uint64_t uval = config["metadata_type_id"].get<uint64_t>();
      if (uval > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        if (error) *error = "config.metadata_type_id exceeds int32 range";
        return -2;
      }
      requested.metadata_type_id = static_cast<int32_t>(uval);
    } else if (config["metadata_type_id"].is_number_integer()) {
      int64_t ival = config["metadata_type_id"].get<int64_t>();
      if (ival < std::numeric_limits<int32_t>::min() ||
          ival > std::numeric_limits<int32_t>::max()) {
        if (error) *error = "config.metadata_type_id exceeds int32 range";
        return -2;
      }
      requested.metadata_type_id = static_cast<int32_t>(ival);
    } else {
      if (error) *error = "config.metadata_type_id must be integer";
      return -2;
    }
  }

  if (config.contains("capacities")) {
    if (!config["capacities"].is_object()) {
      if (error) *error = "config.capacities must be an object";
      return -2;
    }
    for (const auto& [cap_field, cap_val] : config["capacities"].items()) {
      if (!cap_val.is_number_unsigned()) {
        if (error) {
          *error = "Capacity for field '" + cap_field +
                   "' must be positive unsigned integer";
        }
        return -2;
      }
      uint64_t uval = cap_val.get<uint64_t>();
      if (uval == 0 || uval > std::numeric_limits<uint32_t>::max()) {
        if (error) {
          *error = "Capacity for field '" + cap_field +
                   "' must fit a positive uint32";
        }
        return -2;
      }
      requested.capacities[cap_field] = static_cast<uint32_t>(uval);
    }
  }

  return ResolveOutputPoolSpec(*binding, requested, result, error) ? 0 : -2;
}

int ResolveContainedPath(const std::filesystem::path& canonical_root,
                         const std::string& relative_value,
                         const char* field_name, bool check_exists,
                         bool is_directory, std::filesystem::path* resolved,
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
    if (relative_value.rfind("//", 0) == 0 ||
        relative_value.rfind("\\\\", 0) == 0) {
      if (error_msg) {
        *error_msg =
            std::string(field_name) + " contains UNC path: " + relative_value;
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
    if (check_exists) {
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
    } else {
      canon_p = std::filesystem::weakly_canonical(combined, ec);
      if (ec) {
        if (error_msg) {
          *error_msg = "Failed to weakly canonicalize " +
                       std::string(field_name) + ": " + combined.string();
        }
        return -2;
      }
    }

    if (!IsPathWithinRoot(canonical_root, canon_p)) {
      if (error_msg) {
        *error_msg = std::string(field_name) +
                     " symlink escapes model_path: " + canon_p.string();
      }
      return -2;
    }

    if (check_exists) {
      if (is_directory) {
        if (!std::filesystem::is_directory(canon_p, ec) || ec) {
          if (error_msg) {
            *error_msg = std::string(field_name) +
                         " is not a directory: " + canon_p.string();
          }
          return -2;
        }
      } else {
        if (!std::filesystem::is_regular_file(canon_p, ec) || ec) {
          if (error_msg) {
            *error_msg = std::string(field_name) +
                         " must be a regular file: " + canon_p.string();
          }
          return -2;
        }
      }
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

int ResolveRequiredFileUnderRoot(const std::filesystem::path& canonical_root,
                                 const std::string& relative_value,
                                 const char* field_name,
                                 std::filesystem::path* resolved,
                                 std::string* error_msg) noexcept {
  return ResolveContainedPath(canonical_root, relative_value, field_name, true,
                              false, resolved, error_msg);
}

}  // namespace

int OperatorConfigResolver::ResolveModelReferenceUnderRoot(
    const std::filesystem::path& root, const std::string& rel_or_abs,
    const char* field_name, std::filesystem::path* out_path,
    std::string* error_msg) noexcept {
  try {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || ec ||
        !std::filesystem::is_directory(root, ec) || ec) {
      if (error_msg) {
        *error_msg =
            "model_path root must be an existing directory: " + root.string();
      }
      return -2;
    }
    const std::filesystem::path canonical_root =
        std::filesystem::canonical(root, ec);
    if (ec) {
      if (error_msg) {
        *error_msg = "Failed to canonicalize model_path root: " + root.string();
      }
      return -2;
    }
    return ResolveContainedPath(canonical_root, rel_or_abs, field_name, false,
                                false, out_path, error_msg);
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(error_msg, e.what());
    return -2;
  } catch (...) {
    SetDiagnosticNoexcept(error_msg, "Unknown exception");
    return -2;
  }
}

int OperatorConfigResolver::Resolve(const char* model_path,
                                    const char* cfg_file_name,
                                    ResolvedOperatorConfig* result,
                                    std::string* error_msg,
                                    uint32_t max_frame_depth) noexcept {
  try {
    if (!result) {
      if (error_msg) *error_msg = "Null output result pointer";
      return -2;
    }

    if (!model_path || model_path[0] == '\0') {
      if (error_msg) *error_msg = "Null or empty model_path";
      return -2;
    }

    if (!cfg_file_name || cfg_file_name[0] == '\0') {
      if (error_msg) *error_msg = "Null or empty cfg_file_name";
      return -2;
    }

    uint32_t effective_depth =
        max_frame_depth > 0 ? max_frame_depth : kDefaultOutputPoolDepth;
    if (effective_depth > kMaxOutputPoolDepth) {
      if (error_msg) {
        *error_msg = "max_frame_depth (" + std::to_string(effective_depth) +
                     ") exceeds hard limit " +
                     std::to_string(kMaxOutputPoolDepth);
      }
      return -2;
    }

    std::filesystem::path raw_root(model_path);
    std::error_code ec;
    if (!std::filesystem::exists(raw_root, ec) ||
        !std::filesystem::is_directory(raw_root, ec)) {
      if (error_msg) {
        *error_msg =
            "model_path directory does not exist: " + raw_root.string();
      }
      return -2;
    }

    std::filesystem::path canon_root = std::filesystem::canonical(raw_root, ec);
    if (ec) {
      if (error_msg) {
        *error_msg = "Failed to canonicalize model_path: " + raw_root.string();
      }
      return -2;
    }

    // 沙箱解析 cfg_file_name
    std::filesystem::path full_cfg;
    int ret = ResolveRequiredFileUnderRoot(
        canon_root, cfg_file_name, "cfg_file_name", &full_cfg, error_msg);
    if (ret != 0) return ret;

    // 读取并解析部署配置文件 (Schema 1)
    DeploymentIoConfig dep_config;
    std::string dep_err;
    if (!DeploymentIoConfig::ReadFromFile(full_cfg.string(), "operator",
                                          &dep_config, &dep_err,
                                          canon_root.string())) {
      if (error_msg) *error_msg = dep_err;
      return -2;
    }

    // 解析接入绑定计划
    std::unique_ptr<ValidatedIoPlan> io_plan;
    std::string plan_err;
    int plan_ret = IoBindingResolver::ResolveFromConfig(
        dep_config, "operator", canon_root.string(), &io_plan, &plan_err);
    if (plan_ret != 0) {
      if (error_msg) *error_msg = plan_err;
      return plan_ret;
    }

    const auto* out_conv = io_plan->output_converter;
    if (!out_conv) {
      if (error_msg) *error_msg = "Null output converter in validated IO plan";
      return -2;
    }

    // 检查是否有未识别的配置槽名
    for (const auto& [name, value] : dep_config.outputs.items()) {
      bool known = false;
      for (const auto& slot : out_conv->external_slots) {
        if (slot.direction == PortDirection::kOutput &&
            slot.slot_name == name) {
          known = true;
          break;
        }
      }
      if (!known) {
        if (error_msg) *error_msg = "Unknown configured output slot: " + name;
        return -2;
      }
    }

    std::unordered_map<std::string, ResolvedOutputPoolSpec> pool_specs;
    std::unordered_map<std::string, std::string> parameter_texts;
    for (const auto& slot : out_conv->external_slots) {
      if (slot.direction != PortDirection::kOutput) continue;
      if (!dep_config.outputs.contains(slot.slot_name)) {
        if (slot.required) {
          if (error_msg) {
            *error_msg = "Missing allocation configuration for output slot: " +
                         slot.slot_name;
          }
          return -2;
        }
        continue;
      }
      const auto& config = dep_config.outputs[slot.slot_name];
      ResolvedOutputPoolSpec spec;
      std::string parameter_text;
      std::string allocation_error;
      if (ResolveOutputAllocation(config, slot, &spec, &parameter_text,
                                  &allocation_error) != 0) {
        if (error_msg) {
          *error_msg = "Invalid output allocation for slot '" + slot.slot_name +
                       "': " + allocation_error;
        }
        return -2;
      }
      pool_specs.emplace(slot.slot_name, std::move(spec));
      parameter_texts.emplace(slot.slot_name, std::move(parameter_text));
    }

    // 校验单句柄所有输出池总预算
    size_t total_handle_pool_bytes = 0;
    for (const auto& slot : out_conv->external_slots) {
      if (slot.direction != PortDirection::kOutput) continue;
      auto pit = pool_specs.find(slot.slot_name);
      if (pit == pool_specs.end()) continue;
      const auto& pool_spec = pit->second;
      const auto* output_binding =
          OperatorValueTypeRegistry::Instance().GetOutputBinding(
              slot.type_suffix, pool_spec.allocator);
      if (!output_binding ||
          output_binding->direction != IoDirection::kOutput) {
        if (error_msg) {
          *error_msg = "Missing output value binding for suffix '" +
                       slot.type_suffix + "'";
        }
        return -2;
      }
      size_t slot_pool_bytes = 0;
      std::string budget_err;
      if (!ComputeOutputPoolPayloadBytes(*output_binding, pool_spec,
                                         effective_depth, &slot_pool_bytes,
                                         &budget_err)) {
        if (error_msg) {
          *error_msg = "Output pool budget calculation failed: " + budget_err;
        }
        return -2;
      }
      if (!CheckedAdd(total_handle_pool_bytes, slot_pool_bytes,
                      &total_handle_pool_bytes)) {
        if (error_msg) *error_msg = "Handle pool budget addition overflowed";
        return -2;
      }
    }
    if (total_handle_pool_bytes > kMaxHandlePoolPayloadBytes) {
      if (error_msg) {
        *error_msg = "Total output pool payload (" +
                     std::to_string(total_handle_pool_bytes) +
                     " bytes) exceeds per-handle payload budget (" +
                     std::to_string(kMaxHandlePoolPayloadBytes) + " bytes)";
      }
      return -2;
    }

    io_plan->operator_output_specs = pool_specs;

    // 读取 Pipeline JSON 并应用模型覆盖
    std::ifstream pipe_stream(dep_config.resolved_pipe_path);
    if (!pipe_stream.is_open()) {
      if (error_msg) {
        *error_msg =
            "Failed to open pipeline file: " + dep_config.resolved_pipe_path;
      }
      return -2;
    }
    nlohmann::json pipe_json;
    try {
      pipe_stream >> pipe_json;
    } catch (const std::exception& e) {
      if (error_msg) {
        *error_msg = std::string("Failed to parse pipeline JSON: ") + e.what();
      }
      return -2;
    }

    std::unordered_map<std::string, std::string> map_overrides;
    for (const auto& [mid, mstr] : dep_config.model_paths) {
      if (mid.empty() || mstr.empty()) {
        if (error_msg) *error_msg = "Invalid entry in 'model_paths'";
        return -2;
      }
      std::filesystem::path full_mpath;
      ret = ResolveModelReferenceUnderRoot(
          canon_root, mstr, "model_paths entry", &full_mpath, error_msg);
      if (ret != 0) return ret;

      bool matched = false;
      if (pipe_json.contains("models") && pipe_json["models"].is_array()) {
        for (auto& item : pipe_json["models"]) {
          if (item.contains("model_id") && item["model_id"] == mid) {
            map_overrides[mid] = full_mpath.string();
            matched = true;
            break;
          }
        }
      }
      if (!matched) {
        if (error_msg) {
          *error_msg = "Unknown model_id '" + mid + "' in 'model_paths'";
        }
        return -2;
      }
    }

    if (pipe_json.contains("models") && pipe_json["models"].is_array()) {
      for (auto& item : pipe_json["models"]) {
        std::string mid =
            item.contains("model_id") && item["model_id"].is_string()
                ? item["model_id"].get<std::string>()
                : "";
        if (!mid.empty() && map_overrides.find(mid) != map_overrides.end()) {
          item["model_path"] = map_overrides[mid];
        } else if (item.contains("model_path") &&
                   item["model_path"].is_string()) {
          std::string mp = item["model_path"].get<std::string>();
          std::filesystem::path full_mpath;
          ret = ResolveModelReferenceUnderRoot(
              canon_root, mp, "pipeline model_path", &full_mpath, error_msg);
          if (ret != 0) return ret;
          item["model_path"] = full_mpath.string();
        }
      }
    }

    result->conf_path = full_cfg;
    result->pipeline_path = dep_config.resolved_pipe_path;
    result->model_root_path = canon_root;
    result->biz_name = io_plan->binding.biz_name;
    result->io_binding = io_plan->binding.binding_id;
    result->synthetic_pipeline_json = std::move(pipe_json);
    result->output_pool_specs = std::move(pool_specs);
    result->output_parameter_text = std::move(parameter_texts);
    result->io_plan = std::move(io_plan);
    result->input_limits = ResolvedInputLimits{};

    return 0;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(error_msg, e.what());
    return -2;
  } catch (...) {
    SetDiagnosticNoexcept(error_msg, "Unknown exception");
    return -2;
  }
}

}  // namespace llm_edgeflow

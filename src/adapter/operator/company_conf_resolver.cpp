#include "adapter/operator/company_conf_resolver.h"

#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "adapter/biz_adapter_registry.h"

namespace alg_framework {

namespace {

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
    auto rel_check = combined.lexically_relative(canonical_root);
    if (rel_check.empty() || rel_check.string().rfind("..", 0) == 0) {
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

    // 组件级严格包含校验，杜绝前缀混淆 (/root/a vs /root/ab) 与 symlink 逃逸
    auto it_root = canonical_root.begin();
    auto it_p = canon_p.begin();
    while (it_root != canonical_root.end()) {
      if (it_p == canon_p.end() || *it_p != *it_root) {
        if (error_msg) {
          *error_msg = std::string(field_name) +
                       " symlink escapes model_path: " + canon_p.string();
        }
        return -2;
      }
      ++it_root;
      ++it_p;
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
    if (error_msg) {
      *error_msg = std::string("Filesystem exception in ") + field_name + ": " +
                   e.what();
    }
    return -2;
  } catch (...) {
    if (error_msg) {
      *error_msg = std::string("Unknown filesystem exception in ") + field_name;
    }
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

int CompanyConfResolver::ResolveModelReferenceUnderRoot(
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
    if (error_msg) {
      *error_msg = "Filesystem exception resolving model reference: " +
                   std::string(e.what());
    }
    return -2;
  } catch (...) {
    if (error_msg) {
      *error_msg = "Unknown filesystem exception resolving model reference";
    }
    return -2;
  }
}

int CompanyConfResolver::Resolve(const char* model_path,
                                 const char* cfg_file_name,
                                 ResolvedCompanyConfig* result,
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

    // 统一沙箱解析 cfg_file_name (必须存在且为常规文件)
    std::filesystem::path full_cfg;
    int ret = ResolveRequiredFileUnderRoot(
        canon_root, cfg_file_name, "cfg_file_name", &full_cfg, error_msg);
    if (ret != 0) return ret;

    // 读取并解析 .conf JSON
    std::ifstream conf_ifs(full_cfg);
    if (!conf_ifs.is_open()) {
      if (error_msg) *error_msg = "Cannot open conf file: " + full_cfg.string();
      return -2;
    }

    nlohmann::json conf_json;
    try {
      conf_ifs >> conf_json;
    } catch (const std::exception& e) {
      if (error_msg)
        *error_msg = "Invalid JSON in conf file: " + std::string(e.what());
      return -2;
    }

    if (!conf_json.is_object()) {
      if (error_msg) *error_msg = "Conf root must be a JSON object";
      return -2;
    }

    if (conf_json.size() != 1 || !conf_json.contains("data") ||
        !conf_json["data"].is_object()) {
      if (error_msg) {
        *error_msg =
            "Conf root must contain only the required object field 'data'";
      }
      return -2;
    }
    const nlohmann::json* data_obj = &conf_json["data"];
    static const std::unordered_set<std::string> kAllowedDataFields = {
        "pipe_path", "model_paths", "mem_que"};
    for (auto it = data_obj->begin(); it != data_obj->end(); ++it) {
      if (kAllowedDataFields.find(it.key()) == kAllowedDataFields.end()) {
        if (error_msg) {
          *error_msg = "Unknown field in conf data: '" + it.key() + "'";
        }
        return -2;
      }
    }

    if (!data_obj->contains("pipe_path") ||
        !(*data_obj)["pipe_path"].is_string()) {
      if (error_msg) *error_msg = "Missing or invalid 'pipe_path' in conf file";
      return -2;
    }

    std::string pipe_rel = (*data_obj)["pipe_path"].get<std::string>();
    std::filesystem::path full_pipe;
    ret = ResolveRequiredFileUnderRoot(canon_root, pipe_rel, "pipe_path",
                                       &full_pipe, error_msg);
    if (ret != 0) return ret;

    std::ifstream pipe_ifs(full_pipe);
    if (!pipe_ifs.is_open()) {
      if (error_msg)
        *error_msg = "Cannot open pipeline JSON file: " + full_pipe.string();
      return -2;
    }

    nlohmann::json pipe_json;
    try {
      pipe_ifs >> pipe_json;
    } catch (const std::exception& e) {
      if (error_msg)
        *error_msg = "Invalid JSON in pipeline file: " + std::string(e.what());
      return -2;
    }

    std::string biz_name;
    if (pipe_json.is_object() && pipe_json.contains("biz_name") &&
        pipe_json["biz_name"].is_string()) {
      biz_name = pipe_json["biz_name"].get<std::string>();
    } else {
      if (error_msg)
        *error_msg = "Pipeline JSON must contain string 'biz_name'";
      return -2;
    }

    BizAdapterRegistry::AdapterLookupStatus lookup_status;
    auto adapter = BizAdapterRegistry::Instance().GetAdapterByPipelineName(
        biz_name, &lookup_status);
    if (lookup_status ==
        BizAdapterRegistry::AdapterLookupStatus::kAmbiguousMatch) {
      if (error_msg) {
        *error_msg = "Ambiguous pipeline name '" + biz_name + "'";
      }
      return -5;
    }
    if (!adapter) {
      if (error_msg) {
        *error_msg = "No registered BizAdapter for '" + biz_name + "'";
      }
      return -5;
    }

    const auto* bridge_desc =
        OperatorBizBridgeRegistry::Instance().GetBridge(adapter->BizType());
    if (!bridge_desc) {
      if (error_msg) {
        *error_msg = "No OperatorBizBridgeDescriptor for BizType " +
                     std::to_string(adapter->BizType());
      }
      return -5;
    }

    // 解析 data.mem_que
    if (!data_obj->contains("mem_que") || !(*data_obj)["mem_que"].is_object()) {
      if (error_msg) *error_msg = "Missing required 'mem_que' object in conf";
      return -2;
    }

    const auto& mem_que = (*data_obj)["mem_que"];
    if (!mem_que.contains("type") || !mem_que["type"].is_string()) {
      if (error_msg) *error_msg = "Missing required 'type' string in mem_que";
      return -2;
    }

    std::string mem_type = mem_que["type"].get<std::string>();
    bool type_matched = false;
    for (const auto& out_slot : bridge_desc->output_slots) {
      if (out_slot.type_suffix == mem_type) {
        type_matched = true;
        break;
      }
    }
    if (!type_matched) {
      if (error_msg) {
        *error_msg = "mem_que.type '" + mem_type + "' does not match biz '" +
                     biz_name + "' output slot";
      }
      return -2;
    }

    const auto* pool_binding =
        OperatorValueTypeRegistry::Instance().GetBindingBySuffix(mem_type);
    if (!pool_binding || pool_binding->canonical_suffix != mem_type ||
        pool_binding->direction != IoDirection::kOutput) {
      if (error_msg) {
        *error_msg = "No canonical output value binding for mem_que.type '" +
                     mem_type + "'";
      }
      return -2;
    }

    ResolvedOutputPoolSpec requested_pool_spec;
    requested_pool_spec.type = mem_type;

    if (mem_que.contains("meta_num")) {
      if (!mem_que["meta_num"].is_number_unsigned()) {
        if (error_msg)
          *error_msg = "mem_que.meta_num must be non-negative integer";
        return -2;
      }
      uint64_t mnum = mem_que["meta_num"].get<uint64_t>();
      if (mnum > std::numeric_limits<uint32_t>::max()) {
        if (error_msg) *error_msg = "mem_que.meta_num exceeds uint32 range";
        return -2;
      }
      requested_pool_spec.meta_num = static_cast<uint32_t>(mnum);
    }

    if (mem_que.contains("metadata_type_id")) {
      if (!mem_que["metadata_type_id"].is_number_integer()) {
        if (error_msg) *error_msg = "mem_que.metadata_type_id must be integer";
        return -2;
      }
      requested_pool_spec.metadata_type_id =
          mem_que["metadata_type_id"].get<int32_t>();
    }

    if (mem_que.contains("capacities")) {
      if (!mem_que["capacities"].is_object()) {
        if (error_msg) *error_msg = "mem_que.capacities must be an object";
        return -2;
      }
      for (const auto& [cap_field, cap_val] : mem_que["capacities"].items()) {
        if (!cap_val.is_number_unsigned()) {
          if (error_msg) {
            *error_msg = "Capacity for field '" + cap_field +
                         "' must be positive unsigned integer";
          }
          return -2;
        }
        uint64_t uval = cap_val.get<uint64_t>();
        if (uval == 0 || uval > std::numeric_limits<uint32_t>::max()) {
          if (error_msg) {
            *error_msg = "Capacity for field '" + cap_field +
                         "' must fit a positive uint32";
          }
          return -2;
        }
        requested_pool_spec.capacities[cap_field] = static_cast<uint32_t>(uval);
      }
    }

    ResolvedOutputPoolSpec pool_spec;
    std::string spec_error;
    if (!ResolveOutputPoolSpec(*pool_binding, requested_pool_spec, &pool_spec,
                               &spec_error)) {
      if (error_msg) {
        *error_msg = "Invalid output pool configuration: " + spec_error;
      }
      return -2;
    }

    // 计算实际深度下的单句柄所有输出池总预算校验 (Checked Add/Multiply)
    size_t total_handle_pool_bytes = 0;
    for (const auto& out_slot : bridge_desc->output_slots) {
      const auto* output_binding =
          OperatorValueTypeRegistry::Instance().GetBindingBySuffix(
              out_slot.type_suffix);
      if (!output_binding ||
          output_binding->direction != IoDirection::kOutput) {
        if (error_msg) {
          *error_msg = "Missing output value binding for suffix '" +
                       out_slot.type_suffix + "'";
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

    // 1. 严格预检 Pipeline JSON 中的原始模型路径。模型最终文件可以尚未
    // 部署，但引用本身必须是非空相对路径且不能经现存 symlink 前缀逃逸。
    if (pipe_json.contains("models") && pipe_json["models"].is_array()) {
      for (const auto& item : pipe_json["models"]) {
        if (!item.contains("model_path")) continue;
        if (!item["model_path"].is_string()) {
          if (error_msg) {
            *error_msg = "Pipeline JSON model_path must be a string";
          }
          return -2;
        }
        const std::string mp = item["model_path"].get<std::string>();
        std::filesystem::path ignored;
        ret = ResolveModelReferenceUnderRoot(
            canon_root, mp, "pipeline model_path", &ignored, error_msg);
        if (ret != 0) return ret;
      }
    }

    // 2. 解析与校验规范 model_id -> model_path 映射
    std::unordered_map<std::string, std::string> map_overrides;
    if (data_obj->contains("model_paths")) {
      if (!(*data_obj)["model_paths"].is_object()) {
        if (error_msg) *error_msg = "'model_paths' in conf must be an object";
        return -2;
      }
      for (const auto& [mid, mval] : (*data_obj)["model_paths"].items()) {
        if (mid.empty() || !mval.is_string()) {
          if (error_msg) *error_msg = "Invalid entry in 'model_paths'";
          return -2;
        }
        std::string mstr = mval.get<std::string>();
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
    }

    // 3. 全量规范化模型路径 (将未覆盖项通过沙箱解析为绝对规范路径)
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
    result->pipeline_path = full_pipe;
    result->model_root_path = canon_root;
    result->biz_name = biz_name;
    result->biz_type = adapter->BizType();
    result->adapter = adapter;
    result->bridge_descriptor = bridge_desc;
    result->synthetic_pipeline_json = std::move(pipe_json);
    result->output_pool_spec = std::move(pool_spec);

    return 0;
  } catch (const std::exception& e) {
    if (error_msg) *error_msg = std::string("Exception: ") + e.what();
    return -2;
  } catch (...) {
    if (error_msg) *error_msg = "Unknown exception in CompanyConfResolver";
    return -2;
  }
}

}  // namespace alg_framework

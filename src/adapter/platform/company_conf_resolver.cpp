#include "adapter/platform/company_conf_resolver.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "adapter/business_adapter_registry.h"

namespace alg_framework {

namespace {

struct OutputFieldCapacityConfig {
  uint32_t default_capacity;
  uint32_t max_capacity;
};

const std::unordered_map<
    std::string, std::unordered_map<std::string, OutputFieldCapacityConfig>>&
GetOutputCapacityConfigs() {
  static const std::unordered_map<
      std::string, std::unordered_map<std::string, OutputFieldCapacityConfig>>
      kConfigs = {
          {"od_out", {{"result_json", {2047, 65536}}}},
          {"keyword_out", {{"match_result_json", {2047, 65536}}}},
          {"entity_out", {{"entities_json", {2047, 65536}}}},
          {"doc_out",
           {{"intent_name", {63, 255}}, {"answer_text", {1023, 65536}}}},
          {"audit_out",
           {{"risk_level", {31, 255}},
            {"matched_policy_clause", {255, 4096}},
            {"audit_verdict_json", {1023, 65536}}}},
          {"audio_out",
           {{"transcribed_text", {511, 16384}},
            {"intent_slot_json", {1023, 65536}}}},
          {"rerank_out", {}},
      };
  return kConfigs;
}

void PopulateDefaultCapacities(const std::string& type_suffix,
                               ResolvedOutputPoolSpec* spec) {
  const auto& all_configs = GetOutputCapacityConfigs();
  auto it = all_configs.find(type_suffix);
  if (it != all_configs.end()) {
    for (const auto& [field, config] : it->second) {
      if (spec->capacities.find(field) == spec->capacities.end()) {
        spec->capacities[field] = config.default_capacity;
      }
    }
  }
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

int CompanyConfResolver::Resolve(const char* model_path,
                                 const char* cfg_file_name, int32_t device_id,
                                 llm_edgeflow::platform::ChipType /*chip_type*/,
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

    const nlohmann::json* data_obj = nullptr;
    if (conf_json.contains("data")) {
      if (!conf_json["data"].is_object()) {
        if (error_msg) *error_msg = "Field 'data' in conf must be an object";
        return -2;
      }
      data_obj = &conf_json["data"];
    } else {
      data_obj = &conf_json;
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

    if (!pipe_json.is_object() || !pipe_json.contains("business_name") ||
        !pipe_json["business_name"].is_string()) {
      if (error_msg)
        *error_msg = "Pipeline JSON must contain string 'business_name'";
      return -2;
    }

    std::string business_name = pipe_json["business_name"].get<std::string>();

    BusinessAdapterRegistry::AdapterLookupStatus lookup_status;
    auto adapter = BusinessAdapterRegistry::Instance().GetAdapterByPipelineName(
        business_name, &lookup_status);
    if (lookup_status ==
        BusinessAdapterRegistry::AdapterLookupStatus::kAmbiguousMatch) {
      if (error_msg) {
        *error_msg = "Ambiguous pipeline name '" + business_name + "'";
      }
      return -5;
    }
    if (!adapter) {
      if (error_msg) {
        *error_msg =
            "No registered BusinessAdapter for '" + business_name + "'";
      }
      return -5;
    }

    const auto* bridge_desc =
        PlatformBusinessBridgeRegistry::Instance().GetBridge(
            adapter->BizType());
    if (!bridge_desc) {
      if (error_msg) {
        *error_msg = "No PlatformBusinessBridgeDescriptor for BizType " +
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
        *error_msg = "mem_que.type '" + mem_type +
                     "' does not match business '" + business_name +
                     "' output slot";
      }
      return -2;
    }

    ResolvedOutputPoolSpec pool_spec;
    pool_spec.type = mem_type;

    if (mem_que.contains("meta_num")) {
      if (!mem_que["meta_num"].is_number_unsigned()) {
        if (error_msg)
          *error_msg = "mem_que.meta_num must be non-negative integer";
        return -2;
      }
      uint64_t mnum = mem_que["meta_num"].get<uint64_t>();
      if (mnum > 65536) {
        if (error_msg) *error_msg = "mem_que.meta_num exceeds max limit 65536";
        return -2;
      }
      pool_spec.meta_num = static_cast<uint32_t>(mnum);
    }

    if (mem_que.contains("metadata_type_id")) {
      if (!mem_que["metadata_type_id"].is_number_integer()) {
        if (error_msg) *error_msg = "mem_que.metadata_type_id must be integer";
        return -2;
      }
      pool_spec.metadata_type_id = mem_que["metadata_type_id"].get<int32_t>();
    }

    if (pool_spec.meta_num == 0) {
      if (pool_spec.metadata_type_id != 0) {
        if (error_msg) {
          *error_msg = "metadata_type_id must be 0 when meta_num == 0";
        }
        return -2;
      }
    } else {
      if (pool_spec.metadata_type_id == 0 ||
          !FindCompanyAnyType(pool_spec.metadata_type_id)) {
        if (error_msg) {
          *error_msg = "mem_que.metadata_type_id " +
                       std::to_string(pool_spec.metadata_type_id) +
                       " is invalid or not whitelisted";
        }
        return -2;
      }
    }

    const auto& all_configs = GetOutputCapacityConfigs();
    auto cfg_it = all_configs.find(mem_type);

    if (mem_que.contains("capacities")) {
      if (!mem_que["capacities"].is_object()) {
        if (error_msg) *error_msg = "mem_que.capacities must be an object";
        return -2;
      }
      for (const auto& [cap_field, cap_val] : mem_que["capacities"].items()) {
        if (cfg_it == all_configs.end() ||
            cfg_it->second.find(cap_field) == cfg_it->second.end()) {
          if (error_msg) {
            *error_msg = "Unknown capacity field '" + cap_field +
                         "' for output type '" + mem_type + "'";
          }
          return -2;
        }
        if (!cap_val.is_number_unsigned()) {
          if (error_msg) {
            *error_msg = "Capacity for field '" + cap_field +
                         "' must be positive unsigned integer";
          }
          return -2;
        }
        uint64_t uval = cap_val.get<uint64_t>();
        if (uval == 0) {
          if (error_msg) {
            *error_msg = "Capacity for field '" + cap_field + "' cannot be 0";
          }
          return -2;
        }
        const auto& field_cfg = cfg_it->second.at(cap_field);
        if (uval > field_cfg.max_capacity) {
          if (error_msg) {
            *error_msg = "Capacity for field '" + cap_field + "' (" +
                         std::to_string(uval) + ") exceeds max hard limit (" +
                         std::to_string(field_cfg.max_capacity) + ")";
          }
          return -2;
        }
        pool_spec.capacities[cap_field] = static_cast<uint32_t>(uval);
      }
    }

    PopulateDefaultCapacities(mem_type, &pool_spec);

    // 计算实际深度下的单句柄所有输出池总预算校验 (Checked Add/Multiply)
    size_t total_handle_pool_bytes = 0;
    for (const auto& out_slot : bridge_desc->output_slots) {
      size_t slot_pool_bytes = 0;
      std::string budget_err;
      if (!ComputeOutputPoolBytes(out_slot.type_suffix, pool_spec,
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
    if (total_handle_pool_bytes > kMaxHandlePoolMemoryBytes) {
      if (error_msg) {
        *error_msg = "Total output pool memory (" +
                     std::to_string(total_handle_pool_bytes) +
                     " bytes) exceeds per-handle budget (" +
                     std::to_string(kMaxHandlePoolMemoryBytes) + " bytes)";
      }
      return -2;
    }

    // 1. 严格预检 Pipeline JSON 中的原始模型路径 (禁止任何原始绝对路径)
    if (pipe_json.contains("models") && pipe_json["models"].is_array()) {
      for (const auto& item : pipe_json["models"]) {
        if (item.contains("model_path") && item["model_path"].is_string()) {
          std::string mp = item["model_path"].get<std::string>();
          if (!mp.empty()) {
            if (mp[0] == '/' || mp[0] == '\\' ||
                (mp.size() >= 2 &&
                 ((mp[0] >= 'a' && mp[0] <= 'z') ||
                  (mp[0] >= 'A' && mp[0] <= 'Z')) &&
                 mp[1] == ':') ||
                mp.rfind("//", 0) == 0 || mp.rfind("\\\\", 0) == 0) {
              if (error_msg) {
                *error_msg =
                    "Pipeline JSON contains absolute model_path: " + mp;
              }
              return -2;
            }
            std::filesystem::path rel_mp(mp);
            if (rel_mp.is_absolute() || rel_mp.has_root_name() ||
                rel_mp.has_root_directory()) {
              if (error_msg) {
                *error_msg =
                    "Pipeline JSON contains absolute root in model_path: " + mp;
              }
              return -2;
            }
            std::filesystem::path comb =
                (canon_root / rel_mp).lexically_normal();
            auto rel_chk = comb.lexically_relative(canon_root);
            if (rel_chk.empty() || rel_chk.string().rfind("..", 0) == 0) {
              if (error_msg) {
                *error_msg =
                    "Pipeline JSON model_path escapes deployment root: " + mp;
              }
              return -2;
            }
          }
        }
      }
    }

    // 2. 解析与校验模型路径 (model_path 与 model_paths)
    std::unordered_set<std::string> overridden_model_ids;
    bool has_single_model_override = false;
    std::string single_override_path;

    size_t model_count = 0;
    if (pipe_json.contains("models") && pipe_json["models"].is_array()) {
      model_count = pipe_json["models"].size();
    }

    if (data_obj->contains("model_path")) {
      if (!(*data_obj)["model_path"].is_string()) {
        if (error_msg) *error_msg = "'model_path' in conf must be a string";
        return -2;
      }
      std::string single_mpath = (*data_obj)["model_path"].get<std::string>();
      std::filesystem::path full_mpath;
      ret = ResolveRequiredFileUnderRoot(canon_root, single_mpath, "model_path",
                                         &full_mpath, error_msg);
      if (ret != 0) return ret;

      if (model_count == 1) {
        has_single_model_override = true;
        single_override_path = full_mpath.string();
      } else if (model_count > 1) {
        if (error_msg) {
          *error_msg = "Conf specifies single 'model_path' but pipeline has " +
                       std::to_string(model_count) + " models";
        }
        return -2;
      }
    }

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
        ret = ResolveRequiredFileUnderRoot(
            canon_root, mstr, "model_paths entry", &full_mpath, error_msg);
        if (ret != 0) return ret;

        bool matched = false;
        if (pipe_json.contains("models") && pipe_json["models"].is_array()) {
          for (auto& item : pipe_json["models"]) {
            if (item.contains("model_id") && item["model_id"] == mid) {
              map_overrides[mid] = full_mpath.string();
              overridden_model_ids.insert(mid);
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
        if (has_single_model_override) {
          item["model_path"] = single_override_path;
        } else if (!mid.empty() &&
                   map_overrides.find(mid) != map_overrides.end()) {
          item["model_path"] = map_overrides[mid];
        } else if (item.contains("model_path") &&
                   item["model_path"].is_string()) {
          std::string mp = item["model_path"].get<std::string>();
          if (!mp.empty()) {
            std::filesystem::path full_mpath;
            ret = ResolveRequiredFileUnderRoot(
                canon_root, mp, "pipeline model_path", &full_mpath, error_msg);
            if (ret != 0) return ret;
            item["model_path"] = full_mpath.string();
          }
        }
        if (device_id >= 0) {
          if (item.contains("config") && item["config"].is_object()) {
            item["config"]["device_id"] = device_id;
          }
        }
      }
    }

    result->conf_path = full_cfg;
    result->pipeline_path = full_pipe;
    result->model_root_path = canon_root;
    result->business_name = business_name;
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

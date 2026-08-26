#include "adapter/platform/company_conf_resolver.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>

#include "adapter/business_adapter_registry.h"

namespace alg_framework {

namespace {

void PopulateDefaultCapacities(const std::string& type_suffix,
                               ResolvedOutputPoolSpec* spec) {
  if (type_suffix == "od_out") {
    if (spec->capacities.find("result_json") == spec->capacities.end()) {
      spec->capacities["result_json"] = 2047;
    }
  } else if (type_suffix == "keyword_out") {
    if (spec->capacities.find("match_result_json") == spec->capacities.end()) {
      spec->capacities["match_result_json"] = 2047;
    }
  } else if (type_suffix == "entity_out") {
    if (spec->capacities.find("entities_json") == spec->capacities.end()) {
      spec->capacities["entities_json"] = 2047;
    }
  } else if (type_suffix == "doc_out") {
    if (spec->capacities.find("intent_name") == spec->capacities.end()) {
      spec->capacities["intent_name"] = 63;
    }
    if (spec->capacities.find("answer_text") == spec->capacities.end()) {
      spec->capacities["answer_text"] = 1023;
    }
  } else if (type_suffix == "audit_out") {
    if (spec->capacities.find("risk_level") == spec->capacities.end()) {
      spec->capacities["risk_level"] = 31;
    }
    if (spec->capacities.find("matched_policy_clause") ==
        spec->capacities.end()) {
      spec->capacities["matched_policy_clause"] = 255;
    }
    if (spec->capacities.find("audit_verdict_json") == spec->capacities.end()) {
      spec->capacities["audit_verdict_json"] = 1023;
    }
  } else if (type_suffix == "audio_out") {
    if (spec->capacities.find("transcribed_text") == spec->capacities.end()) {
      spec->capacities["transcribed_text"] = 511;
    }
    if (spec->capacities.find("intent_slot_json") == spec->capacities.end()) {
      spec->capacities["intent_slot_json"] = 1023;
    }
  }
}

std::unordered_set<std::string> GetAllowedCapacityFields(
    const std::string& type_suffix) {
  if (type_suffix == "od_out") return {"result_json"};
  if (type_suffix == "keyword_out") return {"match_result_json"};
  if (type_suffix == "entity_out") return {"entities_json"};
  if (type_suffix == "doc_out") return {"intent_name", "answer_text"};
  if (type_suffix == "audit_out")
    return {"risk_level", "matched_policy_clause", "audit_verdict_json"};
  if (type_suffix == "audio_out")
    return {"transcribed_text", "intent_slot_json"};
  return {};
}

}  // namespace

int CompanyConfResolver::Resolve(const char* model_path,
                                 const char* cfg_file_name, int32_t device_id,
                                 llm_edgeflow::platform::ChipType /*chip_type*/,
                                 ResolvedCompanyConfig* result,
                                 std::string* error_msg) noexcept {
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

    std::filesystem::path root_path(model_path);
    if (!std::filesystem::exists(root_path) ||
        !std::filesystem::is_directory(root_path)) {
      if (error_msg) {
        *error_msg =
            "model_path directory does not exist: " + root_path.string();
      }
      return -2;
    }

    std::filesystem::path rel_cfg(cfg_file_name);
    if (rel_cfg.is_absolute() ||
        (cfg_file_name[0] == '/' || cfg_file_name[0] == '\\')) {
      if (error_msg) {
        *error_msg = "cfg_file_name must be a relative path: " +
                     std::string(cfg_file_name);
      }
      return -2;
    }

    std::filesystem::path full_cfg = (root_path / rel_cfg).lexically_normal();
    auto rel_check = full_cfg.lexically_relative(root_path);
    if (rel_check.empty() || rel_check.string().rfind("..", 0) == 0) {
      if (error_msg) {
        *error_msg =
            "cfg_file_name escapes model_path: " + std::string(cfg_file_name);
      }
      return -2;
    }

    if (!std::filesystem::exists(full_cfg)) {
      if (error_msg) {
        *error_msg = "Config file does not exist: " + full_cfg.string();
      }
      return -2;
    }

    // 校验符号链接逃逸
    auto canon_root = std::filesystem::canonical(root_path);
    auto canon_cfg = std::filesystem::canonical(full_cfg);
    auto canon_rel = canon_cfg.lexically_relative(canon_root);
    if (canon_rel.empty() || canon_rel.string().rfind("..", 0) == 0) {
      if (error_msg) {
        *error_msg =
            "Config file symlink escapes model_path: " + full_cfg.string();
      }
      return -2;
    }

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
    if (pipe_rel.empty()) {
      if (error_msg) *error_msg = "'pipe_path' cannot be empty";
      return -2;
    }

    std::filesystem::path full_pipe = (root_path / pipe_rel).lexically_normal();
    if (!std::filesystem::exists(full_pipe)) {
      // 容错：如果 root_path 下不存在，尝试相对 full_cfg 的目录
      std::filesystem::path cfg_dir_pipe =
          (full_cfg.parent_path() / pipe_rel).lexically_normal();
      if (std::filesystem::exists(cfg_dir_pipe)) {
        full_pipe = cfg_dir_pipe;
      } else {
        if (error_msg) {
          *error_msg = "Pipeline JSON does not exist: " + full_pipe.string();
        }
        return -2;
      }
    }

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
      if (!mem_que["meta_num"].is_number_integer() ||
          mem_que["meta_num"].get<int32_t>() < 0) {
        if (error_msg)
          *error_msg = "mem_que.meta_num must be non-negative integer";
        return -2;
      }
      pool_spec.meta_num =
          static_cast<uint32_t>(mem_que["meta_num"].get<int32_t>());
    }

    if (mem_que.contains("metadata_type_id")) {
      if (!mem_que["metadata_type_id"].is_number_integer()) {
        if (error_msg) *error_msg = "mem_que.metadata_type_id must be integer";
        return -2;
      }
      pool_spec.metadata_type_id = mem_que["metadata_type_id"].get<int32_t>();
    }

    if (pool_spec.meta_num == 0 && pool_spec.metadata_type_id != 0) {
      if (error_msg) {
        *error_msg = "metadata_type_id must be 0 when meta_num == 0";
      }
      return -2;
    }

    auto allowed_fields = GetAllowedCapacityFields(mem_type);
    if (mem_que.contains("capacities")) {
      if (!mem_que["capacities"].is_object()) {
        if (error_msg) *error_msg = "mem_que.capacities must be an object";
        return -2;
      }
      for (const auto& [cap_field, cap_val] : mem_que["capacities"].items()) {
        if (allowed_fields.find(cap_field) == allowed_fields.end()) {
          if (error_msg) {
            *error_msg = "Unknown capacity field '" + cap_field +
                         "' for output type '" + mem_type + "'";
          }
          return -2;
        }
        if (!cap_val.is_number_integer() || cap_val.get<int32_t>() <= 0) {
          if (error_msg) {
            *error_msg = "Capacity for field '" + cap_field +
                         "' must be positive integer";
          }
          return -2;
        }
        pool_spec.capacities[cap_field] =
            static_cast<uint32_t>(cap_val.get<int32_t>());
      }
    }

    PopulateDefaultCapacities(mem_type, &pool_spec);

    // 模型路径覆盖与绝对化
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
      if (single_mpath.empty()) {
        if (error_msg) *error_msg = "'model_path' cannot be empty";
        return -2;
      }
      std::filesystem::path p(single_mpath);
      if (p.is_relative()) {
        p = (root_path / p).lexically_normal();
      }
      if (model_count == 1) {
        pipe_json["models"][0]["model_path"] = p.string();
      } else if (model_count > 1) {
        if (error_msg) {
          *error_msg = "Conf specifies single 'model_path' but pipeline has " +
                       std::to_string(model_count) + " models";
        }
        return -2;
      }
    }

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
        std::filesystem::path p(mstr);
        if (p.is_relative()) {
          p = (root_path / p).lexically_normal();
        }
        bool matched = false;
        if (pipe_json.contains("models") && pipe_json["models"].is_array()) {
          for (auto& item : pipe_json["models"]) {
            if (item.contains("model_id") && item["model_id"] == mid) {
              item["model_path"] = p.string();
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

    // 全量规范化模型路径
    if (pipe_json.contains("models") && pipe_json["models"].is_array()) {
      for (auto& item : pipe_json["models"]) {
        if (item.contains("model_path") && item["model_path"].is_string()) {
          std::string mp = item["model_path"].get<std::string>();
          if (!mp.empty()) {
            std::filesystem::path p(mp);
            if (p.is_relative()) {
              p = (root_path / p).lexically_normal();
              item["model_path"] = p.string();
            }
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
    return -99;
  } catch (...) {
    if (error_msg) *error_msg = "Unknown exception in CompanyConfResolver";
    return -100;
  }
}

}  // namespace alg_framework

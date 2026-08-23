#include "adapter/platform/company_conf_resolver.h"

#include <fstream>
#include <iostream>
#include <sstream>

#include "adapter/business_adapter_registry.h"

namespace alg_framework {

bool CompanyConfResolver::Resolve(
    const std::string& conf_file,
    const llm_edgeflow::platform::PlatformConfig& platform_config,
    ResolvedCompanyConfig* result, std::string* error_msg) noexcept {
  try {
    if (!result) {
      if (error_msg) *error_msg = "Null output result pointer";
      return false;
    }

    if (conf_file.empty()) {
      if (error_msg) *error_msg = "Empty conf_file path";
      return false;
    }

    std::filesystem::path conf_path(conf_file);
    if (!std::filesystem::exists(conf_path)) {
      if (error_msg) {
        *error_msg = "Conf file does not exist: " + conf_path.string();
      }
      return false;
    }

    std::filesystem::path base_dir = conf_path.parent_path();
    if (base_dir.empty()) {
      base_dir = std::filesystem::current_path();
    }

    // 1. 读取并解析 .conf
    std::ifstream conf_ifs(conf_path);
    if (!conf_ifs.is_open()) {
      if (error_msg) {
        *error_msg = "Cannot open conf file: " + conf_path.string();
      }
      return false;
    }

    nlohmann::json conf_json;
    try {
      conf_ifs >> conf_json;
    } catch (const std::exception& e) {
      if (error_msg) {
        *error_msg = "Invalid JSON in conf file: " + std::string(e.what());
      }
      return false;
    }

    if (!conf_json.is_object()) {
      if (error_msg) {
        *error_msg = "Conf root must be a JSON object";
      }
      return false;
    }

    // 2. 提取 pipe_path 与 model_path
    std::string pipe_rel_path;
    const nlohmann::json* data_obj = nullptr;
    if (conf_json.contains("data") && conf_json["data"].is_object()) {
      data_obj = &conf_json["data"];
    } else {
      data_obj = &conf_json;
    }

    if (data_obj->contains("pipe_path") &&
        (*data_obj)["pipe_path"].is_string()) {
      pipe_rel_path = (*data_obj)["pipe_path"].get<std::string>();
    } else {
      if (error_msg) {
        *error_msg = "Missing 'pipe_path' in conf file";
      }
      return false;
    }

    std::filesystem::path resolved_pipe_path = pipe_rel_path;
    if (resolved_pipe_path.is_relative()) {
      resolved_pipe_path = base_dir / resolved_pipe_path;
    }

    if (!std::filesystem::exists(resolved_pipe_path)) {
      if (error_msg) {
        *error_msg = "Pipeline file referenced by conf does not exist: " +
                     resolved_pipe_path.string();
      }
      return false;
    }

    // 3. 读取 Pipeline JSON
    std::ifstream pipe_ifs(resolved_pipe_path);
    if (!pipe_ifs.is_open()) {
      if (error_msg) {
        *error_msg =
            "Cannot open pipeline JSON file: " + resolved_pipe_path.string();
      }
      return false;
    }

    nlohmann::json pipe_json;
    try {
      pipe_ifs >> pipe_json;
    } catch (const std::exception& e) {
      if (error_msg) {
        *error_msg = "Invalid JSON in pipeline file: " + std::string(e.what());
      }
      return false;
    }

    if (!pipe_json.is_object() || !pipe_json.contains("business_name") ||
        !pipe_json["business_name"].is_string()) {
      if (error_msg) {
        *error_msg =
            "Pipeline JSON must be an object with valid 'business_name'";
      }
      return false;
    }

    std::string business_name = pipe_json["business_name"].get<std::string>();

    // 4. 反查唯一匹配的 IBusinessAdapter
    auto adapter = BusinessAdapterRegistry::Instance().GetAdapterByPipelineName(
        business_name);
    if (!adapter) {
      if (error_msg) {
        *error_msg = "No registered BusinessAdapter matches pipeline '" +
                     business_name + "'";
      }
      return false;
    }

    // 5. 模型路径覆盖规则 (Section 5.4)
    size_t model_count = 0;
    if (pipe_json.contains("models") && pipe_json["models"].is_array()) {
      model_count = pipe_json["models"].size();
    }

    if (data_obj->contains("model_path") &&
        (*data_obj)["model_path"].is_string()) {
      std::string single_path_str =
          (*data_obj)["model_path"].get<std::string>();
      if (!single_path_str.empty()) {
        std::filesystem::path single_p(single_path_str);
        if (single_p.is_relative()) {
          single_p = base_dir / single_p;
        }

        if (model_count == 0) {
          // Pipeline 没有模型时，忽略 .conf 中的单模型路径
        } else if (model_count == 1) {
          pipe_json["models"][0]["model_path"] = single_p.string();
        } else {
          // Pipeline 有多个模型时，单一 model_path 含义不明确，拒绝
          if (error_msg) {
            *error_msg = "Conf specifies a single 'model_path' but pipeline '" +
                         business_name + "' contains " +
                         std::to_string(model_count) +
                         " models. Please use 'model_paths' mapping instead.";
          }
          return false;
        }
      }
    }

    if (data_obj->contains("model_paths") &&
        (*data_obj)["model_paths"].is_object()) {
      for (const auto& [mid, mpath_val] : (*data_obj)["model_paths"].items()) {
        if (!mpath_val.is_string()) continue;
        std::string mpath_str = mpath_val.get<std::string>();
        std::filesystem::path mp(mpath_str);
        if (mp.is_relative()) {
          mp = base_dir / mp;
        }

        bool matched = false;
        if (pipe_json.contains("models") && pipe_json["models"].is_array()) {
          for (auto& model_item : pipe_json["models"]) {
            if (model_item.contains("model_id") &&
                model_item["model_id"] == mid) {
              model_item["model_path"] = mp.string();
              matched = true;
              break;
            }
          }
        }

        if (!matched) {
          if (error_msg) {
            *error_msg = "Unknown model_id '" + mid +
                         "' in conf 'model_paths' for pipeline '" +
                         business_name + "'";
          }
          return false;
        }
      }
    }

    // 6. 设备 ID 覆盖
    if (platform_config.device_id >= 0 && pipe_json.contains("models") &&
        pipe_json["models"].is_array()) {
      for (auto& model_item : pipe_json["models"]) {
        if (model_item.contains("config") && model_item["config"].is_object()) {
          model_item["config"]["device_id"] = platform_config.device_id;
        }
      }
    }

    result->conf_path = conf_path;
    result->pipeline_path = resolved_pipe_path;
    result->business_name = business_name;
    result->biz_type = adapter->BizType();
    result->adapter = adapter;
    result->synthetic_pipeline_json = std::move(pipe_json);
    return true;
  } catch (const std::exception& e) {
    if (error_msg) *error_msg = std::string("Exception: ") + e.what();
    return false;
  } catch (...) {
    if (error_msg) *error_msg = "Unknown exception in CompanyConfResolver";
    return false;
  }
}

}  // namespace alg_framework

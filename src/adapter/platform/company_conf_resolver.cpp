#include "adapter/platform/company_conf_resolver.h"

#include <fstream>
#include <iostream>
#include <sstream>

#include "adapter/business_adapter_registry.h"

namespace alg_framework {

int CompanyConfResolver::Resolve(
    const std::string& conf_file,
    const llm_edgeflow::platform::PlatformConfig& platform_config,
    ResolvedCompanyConfig* result, std::string* error_msg) noexcept {
  try {
    if (!result) {
      if (error_msg) *error_msg = "Null output result pointer";
      return -2;
    }

    if (conf_file.empty()) {
      if (error_msg) *error_msg = "Empty conf_file path";
      return -2;
    }

    std::filesystem::path raw_conf_path(conf_file);
    if (!std::filesystem::exists(raw_conf_path)) {
      if (error_msg) {
        *error_msg = "Conf file does not exist: " + raw_conf_path.string();
      }
      return -2;
    }

    // 1. 规范化 .conf 绝对路径
    std::filesystem::path conf_path =
        std::filesystem::absolute(raw_conf_path).lexically_normal();
    std::filesystem::path base_dir = conf_path.parent_path();

    // 2. 读取并严格解析 .conf JSON
    std::ifstream conf_ifs(conf_path);
    if (!conf_ifs.is_open()) {
      if (error_msg) {
        *error_msg = "Cannot open conf file: " + conf_path.string();
      }
      return -2;
    }

    nlohmann::json conf_json;
    try {
      conf_ifs >> conf_json;
    } catch (const std::exception& e) {
      if (error_msg) {
        *error_msg =
            "Invalid JSON syntax in conf file: " + std::string(e.what());
      }
      return -2;
    }

    if (!conf_json.is_object()) {
      if (error_msg) *error_msg = "Conf root must be a JSON object";
      return -2;
    }

    // 3. 提取 pipe_path 与 model_path/model_paths
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

    if (!data_obj->contains("pipe_path")) {
      if (error_msg)
        *error_msg = "Missing required field 'pipe_path' in conf file";
      return -2;
    }
    if (!(*data_obj)["pipe_path"].is_string()) {
      if (error_msg) *error_msg = "Field 'pipe_path' in conf must be a string";
      return -2;
    }

    std::string pipe_rel_path = (*data_obj)["pipe_path"].get<std::string>();
    if (pipe_rel_path.empty()) {
      if (error_msg) *error_msg = "Field 'pipe_path' cannot be empty";
      return -2;
    }

    std::filesystem::path resolved_pipe_path(pipe_rel_path);
    if (resolved_pipe_path.is_relative()) {
      resolved_pipe_path = (base_dir / resolved_pipe_path).lexically_normal();
    } else {
      resolved_pipe_path = resolved_pipe_path.lexically_normal();
    }

    if (!std::filesystem::exists(resolved_pipe_path)) {
      if (error_msg) {
        *error_msg = "Pipeline JSON referenced by conf does not exist: " +
                     resolved_pipe_path.string();
      }
      return -2;
    }

    // 4. 读取 Pipeline JSON
    std::ifstream pipe_ifs(resolved_pipe_path);
    if (!pipe_ifs.is_open()) {
      if (error_msg) {
        *error_msg =
            "Cannot open pipeline JSON file: " + resolved_pipe_path.string();
      }
      return -2;
    }

    nlohmann::json pipe_json;
    try {
      pipe_ifs >> pipe_json;
    } catch (const std::exception& e) {
      if (error_msg) {
        *error_msg = "Invalid JSON in pipeline file: " + std::string(e.what());
      }
      return -2;
    }

    if (!pipe_json.is_object() || !pipe_json.contains("business_name") ||
        !pipe_json["business_name"].is_string()) {
      if (error_msg) {
        *error_msg =
            "Pipeline JSON must be an object with valid 'business_name'";
      }
      return -2;
    }

    std::string business_name = pipe_json["business_name"].get<std::string>();

    // 5. 反查唯一匹配的 IBusinessAdapter (Fail-Closed 校验)
    BusinessAdapterRegistry::AdapterLookupStatus lookup_status;
    auto adapter = BusinessAdapterRegistry::Instance().GetAdapterByPipelineName(
        business_name, &lookup_status);
    if (lookup_status ==
        BusinessAdapterRegistry::AdapterLookupStatus::kAmbiguousMatch) {
      if (error_msg) {
        *error_msg = "Ambiguous pipeline name '" + business_name +
                     "': matched multiple registered BusinessAdapters";
      }
      return -5;
    }
    if (!adapter) {
      if (error_msg) {
        *error_msg = "No registered BusinessAdapter matches pipeline name '" +
                     business_name + "'";
      }
      return -5;
    }

    // 6. 校验 PlatformIoDescriptor 绑定
    const auto* io_desc =
        PlatformIoRegistry::Instance().GetDescriptor(adapter->BizType());
    if (!io_desc) {
      if (error_msg) {
        *error_msg = "No PlatformIoDescriptor registered for BizType " +
                     std::to_string(adapter->BizType()) + " (" +
                     adapter->BizName() + ")";
      }
      return -5;
    }

    // 7. 模型路径覆盖规则 (Section 5.4)
    size_t model_count = 0;
    if (pipe_json.contains("models") && pipe_json["models"].is_array()) {
      model_count = pipe_json["models"].size();
    }

    // 7.1 单模型覆盖
    if (data_obj->contains("model_path")) {
      if (!(*data_obj)["model_path"].is_string()) {
        if (error_msg)
          *error_msg = "Field 'model_path' in conf must be a string";
        return -2;
      }
      std::string single_path_str =
          (*data_obj)["model_path"].get<std::string>();
      if (single_path_str.empty()) {
        if (error_msg) *error_msg = "Field 'model_path' cannot be empty string";
        return -2;
      }

      std::filesystem::path single_p(single_path_str);
      if (single_p.is_relative()) {
        single_p = (base_dir / single_p).lexically_normal();
      } else {
        single_p = single_p.lexically_normal();
      }

      if (model_count == 0) {
        // Pipeline 没有声明模型（如纯规则业务），安全忽略
      } else if (model_count == 1) {
        pipe_json["models"][0]["model_path"] = single_p.string();
      } else {
        if (error_msg) {
          *error_msg = "Conf specifies a single 'model_path' but pipeline '" +
                       business_name + "' contains " +
                       std::to_string(model_count) +
                       " models. Must use 'model_paths' mapping instead.";
        }
        return -2;
      }
    }

    // 7.2 多模型映射覆盖
    if (data_obj->contains("model_paths")) {
      if (!(*data_obj)["model_paths"].is_object()) {
        if (error_msg)
          *error_msg = "Field 'model_paths' in conf must be an object";
        return -2;
      }
      for (const auto& [mid, mpath_val] : (*data_obj)["model_paths"].items()) {
        if (mid.empty()) {
          if (error_msg)
            *error_msg = "model_id in 'model_paths' cannot be empty";
          return -2;
        }
        if (!mpath_val.is_string()) {
          if (error_msg) {
            *error_msg = "model_path for model_id '" + mid +
                         "' in 'model_paths' must be a string";
          }
          return -2;
        }
        std::string mpath_str = mpath_val.get<std::string>();
        if (mpath_str.empty()) {
          if (error_msg) {
            *error_msg = "model_path for model_id '" + mid +
                         "' in 'model_paths' cannot be empty";
          }
          return -2;
        }

        std::filesystem::path mp(mpath_str);
        if (mp.is_relative()) {
          mp = (base_dir / mp).lexically_normal();
        } else {
          mp = mp.lexically_normal();
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
          return -2;
        }
      }
    }

    // 7.3 全量规范化：确保 Pipeline JSON 中所有未被覆盖的相对模型路径，也全部按
    // base_dir 绝对化
    if (pipe_json.contains("models") && pipe_json["models"].is_array()) {
      for (auto& model_item : pipe_json["models"]) {
        if (model_item.contains("model_path") &&
            model_item["model_path"].is_string()) {
          std::string mpath = model_item["model_path"].get<std::string>();
          if (!mpath.empty()) {
            std::filesystem::path p(mpath);
            if (p.is_relative()) {
              p = (base_dir / p).lexically_normal();
              model_item["model_path"] = p.string();
            }
          }
        }
      }
    }

    // 8. 设备 ID 覆盖
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
    result->io_descriptor = io_desc;
    result->synthetic_pipeline_json = std::move(pipe_json);
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

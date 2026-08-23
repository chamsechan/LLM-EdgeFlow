#include "adapter/platform/platform_control_registry.h"

#include <cstring>
#include <iostream>

#include "nlohmann/json.hpp"

namespace alg_framework {

int PlatformControlRegistry::ResolveControlParam(
    llm_edgeflow::platform::ControlCommand command, void* control_param,
    int* out_cmd_id, std::string* out_json_str,
    std::string* error_msg) noexcept {
  try {
    if (!out_cmd_id || !out_json_str) {
      if (error_msg) *error_msg = "Null output pointer for control resolution";
      return -2;
    }

    if (!control_param) {
      if (error_msg) *error_msg = "Null control_param pointer";
      return -2;
    }

    int cmd_val = static_cast<int>(command);
    if (cmd_val < 1 || cmd_val > 3) {
      if (error_msg) {
        *error_msg =
            "Unsupported ControlCommand enum value: " + std::to_string(cmd_val);
      }
      return -2;
    }

    *out_cmd_id = cmd_val;

    // 尝试将 control_param 解释为 const char* (JSON 字符串)
    // 或者结构体指针（若为以 const char* 开头的结构体）
    const char* str_ptr = nullptr;
    // 兼容 const char* 直接传入
    str_ptr = static_cast<const char*>(control_param);

    // 简单探测：若首字符为 '{' 或 '['，视为 JSON 字符串
    if (str_ptr && (str_ptr[0] == '{' || str_ptr[0] == '[')) {
      *out_json_str = str_ptr;
    } else {
      // 尝试作为包装结构体（首个字段为 const char* json_str）
      const char** struct_str_ptr = static_cast<const char**>(control_param);
      if (struct_str_ptr && *struct_str_ptr &&
          ((*struct_str_ptr)[0] == '{' || (*struct_str_ptr)[0] == '[')) {
        *out_json_str = *struct_str_ptr;
      } else {
        // 若为普通非空字符串，包装为合法的默认 JSON 负载
        if (str_ptr && strlen(str_ptr) > 0) {
          nlohmann::json j;
          j["param"] = str_ptr;
          *out_json_str = j.dump();
        } else {
          if (error_msg)
            *error_msg = "Cannot parse control_param into valid JSON";
          return -2;
        }
      }
    }

    // 验证 JSON 合法性
    try {
      auto parsed = nlohmann::json::parse(*out_json_str);
      if (!parsed.is_object() && !parsed.is_array()) {
        if (error_msg) *error_msg = "Control JSON must be an object or array";
        return -2;
      }
    } catch (const std::exception& e) {
      if (error_msg) {
        *error_msg = "Invalid JSON in control_param: " + std::string(e.what());
      }
      return -2;
    }

    return 0;
  } catch (const std::exception& e) {
    if (error_msg) *error_msg = std::string("Exception: ") + e.what();
    return -99;
  } catch (...) {
    if (error_msg) *error_msg = "Unknown exception in ResolveControlParam";
    return -100;
  }
}

}  // namespace alg_framework

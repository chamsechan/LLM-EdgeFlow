#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "contracts/config_schema.h"
#include "contracts/json_pointer.h"

namespace llm_edgeflow {

enum class ConfigFieldErrorKind {
  kNotAnObject,
  kUnknownField,
  kMissingField,
  kTypeMismatch,
  kOutOfRange,
  kInvalidEnum,
  kNonFinite,
};

struct ConfigFieldValidationError {
  // 相对路径：顶层为参数名，元素继续写键名或下标，如 "rules/3/pattern"。
  std::string path;
  ConfigFieldErrorKind kind;
  std::string message;
  // 仅 kUnknownField：该层声明的参数名，供给出相近名字建议。
  std::vector<std::string> candidates;
};

inline bool IsValidConfigValueKind(ConfigValueKind kind) noexcept {
  switch (kind) {
    case ConfigValueKind::kString:
    case ConfigValueKind::kInteger:
    case ConfigValueKind::kNumber:
    case ConfigValueKind::kBoolean:
    case ConfigValueKind::kObject:
    case ConfigValueKind::kArray:
    case ConfigValueKind::kMap:
    case ConfigValueKind::kJson:
      return true;
    default:
      return false;
  }
}

namespace detail {

// 不含端点的整数上界可在 double 中精确表示；
// INT64_MAX/UINT64_MAX 转为 double 时会向上舍入到这些值。
inline constexpr double kInt64UpperBound = 9223372036854775808.0;    // 2^63
inline constexpr double kUint64UpperBound = 18446744073709551616.0;  // 2^64

inline bool IsValueBelowMinimum(const nlohmann::json& val, double min_val) {
  if (val.is_number_unsigned()) {
    if (min_val <= 0.0) return false;
    if (min_val >= kUint64UpperBound) return true;
    return val.get<uint64_t>() < static_cast<uint64_t>(std::ceil(min_val));
  }
  if (val.is_number_integer()) {
    if (min_val <= static_cast<double>(std::numeric_limits<int64_t>::min()))
      return false;
    if (min_val >= kInt64UpperBound) return true;
    return val.get<int64_t>() < static_cast<int64_t>(std::ceil(min_val));
  }
  return val.get<double>() < min_val;
}

inline bool IsValueAboveMaximum(const nlohmann::json& val, double max_val) {
  if (val.is_number_unsigned()) {
    if (max_val < 0.0) return true;
    if (max_val >= kUint64UpperBound) return false;
    const uint64_t u = val.get<uint64_t>();
    const uint64_t floor_val = static_cast<uint64_t>(max_val);
    return u > floor_val;
  }
  if (val.is_number_integer()) {
    if (max_val < static_cast<double>(std::numeric_limits<int64_t>::min()))
      return true;
    if (max_val >= kInt64UpperBound) return false;
    const int64_t i = val.get<int64_t>();
    const int64_t floor_val = static_cast<int64_t>(std::floor(max_val));
    return i > floor_val;
  }
  return val.get<double>() > max_val;
}

inline std::string JoinConfigPath(const std::string& base,
                                  std::string_view token) {
  const std::string escaped = EscapeJsonPointer(token);
  return base.empty() ? escaped : base + "/" + escaped;
}

inline bool NormalizeObjectFields(
    const std::vector<ConfigFieldDefinition>& schema,
    const nlohmann::json& input, const std::string& base_path,
    nlohmann::json* normalized,
    std::vector<ConfigFieldValidationError>* errors);

// 按定义校验一个值：类型、范围、枚举，并递归校验数组、映射、结构体元素；
// 通过时 normalized 是补齐默认值后的值。
inline bool NormalizeValue(const ConfigFieldDefinition& def,
                           const nlohmann::json& val, const std::string& path,
                           nlohmann::json* normalized,
                           std::vector<ConfigFieldValidationError>* errors) {
  const auto fail = [&](ConfigFieldErrorKind kind, std::string message) {
    if (errors) errors->push_back({path, kind, std::move(message), {}});
    return false;
  };

  // 非有限数值检查
  if (val.is_number() && !std::isfinite(val.get<double>())) {
    return fail(ConfigFieldErrorKind::kNonFinite,
                "Numeric value must be finite for field: " + path);
  }

  // 类型匹配检查
  bool type_ok = false;
  switch (def.kind) {
    case ConfigValueKind::kString:
      type_ok = val.is_string();
      break;
    case ConfigValueKind::kInteger:
      // 浮点数 (如 2.5) 不符合整型
      type_ok = val.is_number_integer() || val.is_number_unsigned();
      break;
    case ConfigValueKind::kNumber:
      type_ok = val.is_number();
      break;
    case ConfigValueKind::kBoolean:
      type_ok = val.is_boolean();
      break;
    case ConfigValueKind::kObject:
    case ConfigValueKind::kMap:
      type_ok = val.is_object();
      break;
    case ConfigValueKind::kArray:
      type_ok = val.is_array();
      break;
    case ConfigValueKind::kJson:
      type_ok = !val.is_null();
      break;
  }
  if (!type_ok) {
    return fail(ConfigFieldErrorKind::kTypeMismatch,
                "Expected " + std::string(ConfigValueKindName(def.kind)));
  }

  bool ok = true;
  // 数值范围检查
  if (val.is_number()) {
    if (def.minimum.has_value() &&
        detail::IsValueBelowMinimum(val, *def.minimum)) {
      ok = fail(
          ConfigFieldErrorKind::kOutOfRange,
          "Numeric value is below minimum " + std::to_string(*def.minimum));
    } else if (def.maximum.has_value() &&
               detail::IsValueAboveMaximum(val, *def.maximum)) {
      ok =
          fail(ConfigFieldErrorKind::kOutOfRange,
               "Numeric value exceeds maximum " + std::to_string(*def.maximum));
    }
  }

  // 字符串枚举检查
  if (def.kind == ConfigValueKind::kString && !def.enum_values.empty()) {
    const std::string& str_val = val.get_ref<const std::string&>();
    if (std::find(def.enum_values.begin(), def.enum_values.end(), str_val) ==
        def.enum_values.end()) {
      ok = fail(ConfigFieldErrorKind::kInvalidEnum,
                "String value '" + str_val + "' not in allowed enum values");
    }
  }

  nlohmann::json result = val;
  if (def.kind == ConfigValueKind::kArray && def.items) {
    result = nlohmann::json::array();
    for (size_t i = 0; i < val.size(); ++i) {
      nlohmann::json element;
      if (NormalizeValue(*def.items, val[i],
                         JoinConfigPath(path, std::to_string(i)), &element,
                         errors)) {
        result.push_back(std::move(element));
      } else {
        ok = false;
      }
    }
  } else if (def.kind == ConfigValueKind::kMap && def.items) {
    result = nlohmann::json::object();
    for (auto it = val.begin(); it != val.end(); ++it) {
      nlohmann::json element;
      if (NormalizeValue(*def.items, it.value(), JoinConfigPath(path, it.key()),
                         &element, errors)) {
        result[it.key()] = std::move(element);
      } else {
        ok = false;
      }
    }
  } else if (def.kind == ConfigValueKind::kObject && !def.fields.empty()) {
    if (!NormalizeObjectFields(def.fields, val, path, &result, errors)) {
      ok = false;
    }
  }
  if (ok && normalized) *normalized = std::move(result);
  return ok;
}

inline bool NormalizeObjectFields(
    const std::vector<ConfigFieldDefinition>& schema,
    const nlohmann::json& input, const std::string& base_path,
    nlohmann::json* normalized,
    std::vector<ConfigFieldValidationError>* errors) {
  bool ok = true;
  nlohmann::json result = nlohmann::json::object();

  // 1. 未知字段校验
  for (auto it = input.begin(); it != input.end(); ++it) {
    const std::string& key = it.key();
    const bool found =
        std::any_of(schema.begin(), schema.end(),
                    [&](const auto& f) { return f.name == key; });
    if (!found) {
      ok = false;
      if (errors) {
        ConfigFieldValidationError error{JoinConfigPath(base_path, key),
                                         ConfigFieldErrorKind::kUnknownField,
                                         "Unknown config field: " + key,
                                         {}};
        for (const auto& f : schema) error.candidates.push_back(f.name);
        errors->push_back(std::move(error));
      }
    }
  }

  // 2. 已声明字段约束校验与默认值注入
  for (const auto& field : schema) {
    const std::string path = JoinConfigPath(base_path, field.name);
    if (!input.contains(field.name)) {
      if (field.required) {
        ok = false;
        if (errors) {
          errors->push_back({path,
                             ConfigFieldErrorKind::kMissingField,
                             "Missing required config field: " + field.name,
                             {}});
        }
      } else if (!field.default_value.is_null()) {
        result[field.name] = field.default_value;
      }
      continue;
    }
    nlohmann::json value;
    if (NormalizeValue(field, input[field.name], path, &value, errors)) {
      result[field.name] = std::move(value);
    } else {
      ok = false;
    }
  }
  if (ok && normalized) *normalized = std::move(result);
  return ok;
}

}  // namespace detail

inline bool ValidateConfigFieldDefinitions(
    const std::vector<ConfigFieldDefinition>& fields, std::string* error);

namespace detail {

// require_name 为 false 时用于数组、映射的元素定义（名字为空）。
inline bool ValidateFieldDefinition(const ConfigFieldDefinition& field,
                                    bool require_name, std::string* error) {
  if (require_name && field.name.empty()) {
    if (error) *error = "Config field name cannot be empty";
    return false;
  }
  if (!IsValidConfigValueKind(field.kind)) {
    if (error) *error = "Invalid config value kind in: " + field.name;
    return false;
  }

  if ((field.minimum.has_value() && !std::isfinite(*field.minimum)) ||
      (field.maximum.has_value() && !std::isfinite(*field.maximum))) {
    if (error) *error = "Config bounds must be finite in: " + field.name;
    return false;
  }
  if (field.minimum.has_value() || field.maximum.has_value()) {
    if (field.kind != ConfigValueKind::kInteger &&
        field.kind != ConfigValueKind::kNumber) {
      if (error) {
        *error =
            "Min/Max bounds only allowed for Integer or Number: " + field.name;
      }
      return false;
    }
    if (field.minimum.has_value() && field.maximum.has_value() &&
        *field.minimum > *field.maximum) {
      if (error) {
        *error = "Minimum bound cannot be greater than maximum bound in: " +
                 field.name;
      }
      return false;
    }
  }

  if (!field.default_value.is_null()) {
    switch (field.kind) {
      case ConfigValueKind::kInteger: {
        if (!field.default_value.is_number_integer() &&
            !field.default_value.is_number_unsigned()) {
          if (error) {
            *error = "Default value for Integer field must be integer: " +
                     field.name;
          }
          return false;
        }
        if ((field.minimum.has_value() &&
             detail::IsValueBelowMinimum(field.default_value,
                                         *field.minimum)) ||
            (field.maximum.has_value() &&
             detail::IsValueAboveMaximum(field.default_value,
                                         *field.maximum))) {
          if (error) {
            *error = "Default value outside bounds for field: " + field.name;
          }
          return false;
        }
        break;
      }
      case ConfigValueKind::kNumber: {
        if (!field.default_value.is_number()) {
          if (error) {
            *error =
                "Default value for Number field must be numeric: " + field.name;
          }
          return false;
        }
        const double value = field.default_value.get<double>();
        if (!std::isfinite(value)) {
          if (error) {
            *error = "Default numeric value must be finite in: " + field.name;
          }
          return false;
        }
        if ((field.minimum.has_value() && value < *field.minimum) ||
            (field.maximum.has_value() && value > *field.maximum)) {
          if (error) {
            *error = "Default value outside bounds for field: " + field.name;
          }
          return false;
        }
        break;
      }
      case ConfigValueKind::kBoolean:
        if (!field.default_value.is_boolean()) {
          if (error) {
            *error = "Default value for Boolean field must be boolean: " +
                     field.name;
          }
          return false;
        }
        break;
      case ConfigValueKind::kString:
        if (!field.default_value.is_string()) {
          if (error) {
            *error =
                "Default value for String field must be string: " + field.name;
          }
          return false;
        }
        break;
      case ConfigValueKind::kObject:
        if (!field.default_value.is_object()) {
          if (error) {
            *error =
                "Default value for Object field must be object: " + field.name;
          }
          return false;
        }
        break;
      case ConfigValueKind::kMap:
        if (!field.default_value.is_object()) {
          if (error) {
            *error =
                "Default value for Map field must be object: " + field.name;
          }
          return false;
        }
        break;
      case ConfigValueKind::kJson:
        break;
      case ConfigValueKind::kArray:
        if (!field.default_value.is_array()) {
          if (error) {
            *error =
                "Default value for Array field must be array: " + field.name;
          }
          return false;
        }
        break;
      default:
        if (error) *error = "Invalid config value kind in: " + field.name;
        return false;
    }
  }

  if (!field.enum_values.empty()) {
    if (field.kind != ConfigValueKind::kString) {
      if (error) {
        *error = "Enum values only allowed for String kind in: " + field.name;
      }
      return false;
    }
    std::unordered_set<std::string> seen_values;
    for (const auto& value : field.enum_values) {
      if (value.empty() || !seen_values.insert(value).second) {
        if (error) {
          *error = "Empty or duplicate enum value in field: " + field.name;
        }
        return false;
      }
    }
    if (field.default_value.is_string() &&
        seen_values.find(field.default_value.get<std::string>()) ==
            seen_values.end()) {
      if (error) {
        *error = "Default value is not in enum_values for field: " + field.name;
      }
      return false;
    }
  }

  if (field.items) {
    if (field.kind != ConfigValueKind::kArray &&
        field.kind != ConfigValueKind::kMap) {
      if (error) {
        *error =
            "Element definition only allowed for Array or Map: " + field.name;
      }
      return false;
    }
    std::string element_error;
    if (!ValidateFieldDefinition(*field.items, false, &element_error)) {
      if (error) {
        *error = "Invalid element definition of '" + field.name +
                 "': " + element_error;
      }
      return false;
    }
  }
  if (!field.fields.empty()) {
    if (field.kind != ConfigValueKind::kObject) {
      if (error) {
        *error = "Element fields only allowed for Object: " + field.name;
      }
      return false;
    }
    std::string fields_error;
    if (!ValidateConfigFieldDefinitions(field.fields, &fields_error)) {
      if (error) {
        *error = "Invalid fields of '" + field.name + "': " + fields_error;
      }
      return false;
    }
  }
  if (!field.default_value.is_null() &&
      (field.items || !field.fields.empty())) {
    std::vector<ConfigFieldValidationError> default_errors;
    if (!NormalizeValue(field, field.default_value, field.name, nullptr,
                        &default_errors)) {
      if (error) {
        *error = "Default value is invalid for field: " + field.name +
                 (default_errors.empty()
                      ? ""
                      : " (" + default_errors.front().path + ": " +
                            default_errors.front().message + ")");
      }
      return false;
    }
  }
  return true;
}

}  // namespace detail

inline bool ValidateConfigFieldDefinitions(
    const std::vector<ConfigFieldDefinition>& fields, std::string* error) {
  std::unordered_set<std::string> seen_names;
  for (const auto& field : fields) {
    if (field.name.empty()) {
      if (error) *error = "Config field name cannot be empty";
      return false;
    }
    if (!seen_names.insert(field.name).second) {
      if (error) *error = "Duplicate config field name: " + field.name;
      return false;
    }
    if (!detail::ValidateFieldDefinition(field, true, error)) return false;
  }
  return true;
}

// 校验并归一化单个参数值（控制命令的 payload 逐项使用）。
inline bool NormalizeFieldValue(
    const ConfigFieldDefinition& field, const nlohmann::json& value,
    nlohmann::json* normalized,
    std::vector<ConfigFieldValidationError>* errors) {
  return detail::NormalizeValue(
      field, value, detail::JoinConfigPath("", field.name), normalized, errors);
}

inline bool ValidateAndNormalizeFields(
    const std::vector<ConfigFieldDefinition>& schema,
    const nlohmann::json& input, nlohmann::json* normalized,
    std::vector<ConfigFieldValidationError>* errors) {
  if (!input.is_object()) {
    if (errors) {
      errors->push_back({"",
                         ConfigFieldErrorKind::kNotAnObject,
                         "Config must be a JSON object",
                         {}});
    }
    return false;
  }
  return detail::NormalizeObjectFields(schema, input, "", normalized, errors);
}

}  // namespace llm_edgeflow

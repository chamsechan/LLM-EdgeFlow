#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

#include "contracts/config_schema.h"

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
  std::string field_name;
  ConfigFieldErrorKind kind;
  std::string message;
};

inline bool IsValidConfigValueKind(ConfigValueKind kind) noexcept {
  switch (kind) {
    case ConfigValueKind::kString:
    case ConfigValueKind::kInteger:
    case ConfigValueKind::kNumber:
    case ConfigValueKind::kBoolean:
    case ConfigValueKind::kObject:
    case ConfigValueKind::kArray:
      return true;
    default:
      return false;
  }
}

namespace detail {

// Exclusive integer upper bounds are exactly representable in double;
// converting INT64_MAX/UINT64_MAX to double rounds up to these values.
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
          *error = "Min/Max bounds only allowed for Integer or Number: " +
                   field.name;
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
              *error = "Default value for Number field must be numeric: " +
                       field.name;
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
              *error = "Default value for String field must be string: " +
                       field.name;
            }
            return false;
          }
          break;
        case ConfigValueKind::kObject:
          if (!field.default_value.is_object()) {
            if (error) {
              *error = "Default value for Object field must be object: " +
                       field.name;
            }
            return false;
          }
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
          *error =
              "Default value is not in enum_values for field: " + field.name;
        }
        return false;
      }
    }
  }
  return true;
}

inline bool ValidateAndNormalizeFields(
    const std::vector<ConfigFieldDefinition>& schema,
    const nlohmann::json& input, nlohmann::json* normalized,
    std::vector<ConfigFieldValidationError>* errors) {
  if (!input.is_object()) {
    if (errors) {
      errors->push_back({"", ConfigFieldErrorKind::kNotAnObject,
                         "Config must be a JSON object"});
    }
    return false;
  }

  bool ok = true;
  nlohmann::json result = nlohmann::json::object();

  // 1. 未知字段校验
  for (auto it = input.begin(); it != input.end(); ++it) {
    const std::string& key = it.key();
    bool found = std::any_of(schema.begin(), schema.end(),
                             [&](const auto& f) { return f.name == key; });
    if (!found) {
      ok = false;
      if (errors) {
        errors->push_back({key, ConfigFieldErrorKind::kUnknownField,
                           "Unknown config field: " + key});
      }
    }
  }

  // 2. 已声明字段约束校验与默认值注入
  for (const auto& field : schema) {
    if (!input.contains(field.name)) {
      if (field.required) {
        ok = false;
        if (errors) {
          errors->push_back({field.name, ConfigFieldErrorKind::kMissingField,
                             "Missing required config field: " + field.name});
        }
      } else if (!field.default_value.is_null()) {
        result[field.name] = field.default_value;
      }
      continue;
    }

    const auto& val = input[field.name];

    // 非有限数值检查
    if (val.is_number()) {
      double d = val.get<double>();
      if (!std::isfinite(d)) {
        ok = false;
        if (errors) {
          errors->push_back(
              {field.name, ConfigFieldErrorKind::kNonFinite,
               "Numeric value must be finite for field: " + field.name});
        }
        continue;
      }
    }

    // 类型匹配检查
    bool type_ok = false;
    switch (field.kind) {
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
        type_ok = val.is_object();
        break;
      case ConfigValueKind::kArray:
        type_ok = val.is_array();
        break;
    }

    if (!type_ok) {
      ok = false;
      if (errors) {
        errors->push_back(
            {field.name, ConfigFieldErrorKind::kTypeMismatch,
             "Expected " + std::string(ConfigValueKindName(field.kind))});
      }
      continue;
    }

    // 数值范围检查
    if (val.is_number()) {
      if (field.minimum.has_value() &&
          detail::IsValueBelowMinimum(val, *field.minimum)) {
        ok = false;
        if (errors) {
          errors->push_back({field.name, ConfigFieldErrorKind::kOutOfRange,
                             "Numeric value is below minimum " +
                                 std::to_string(*field.minimum)});
        }
      } else if (field.maximum.has_value() &&
                 detail::IsValueAboveMaximum(val, *field.maximum)) {
        ok = false;
        if (errors) {
          errors->push_back({field.name, ConfigFieldErrorKind::kOutOfRange,
                             "Numeric value exceeds maximum " +
                                 std::to_string(*field.maximum)});
        }
      }
    }

    // 字符串枚举检查
    if (field.kind == ConfigValueKind::kString && !field.enum_values.empty() &&
        val.is_string()) {
      std::string str_val = val.get<std::string>();
      if (std::find(field.enum_values.begin(), field.enum_values.end(),
                    str_val) == field.enum_values.end()) {
        ok = false;
        if (errors) {
          errors->push_back(
              {field.name, ConfigFieldErrorKind::kInvalidEnum,
               "String value '" + str_val + "' not in allowed enum values"});
        }
      }
    }

    result[field.name] = val;
  }

  if (ok && normalized) {
    *normalized = std::move(result);
  }
  return ok;
}

}  // namespace llm_edgeflow

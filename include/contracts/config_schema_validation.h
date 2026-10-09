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
  std::string path;
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
    case ConfigValueKind::kMap:
    case ConfigValueKind::kJson:
      return true;
    default:
      return false;
  }
}

namespace detail {

inline bool NormalizeConfigValue(
    const ConfigFieldDefinition& field, const nlohmann::json& value,
    nlohmann::json* normalized, std::vector<ConfigFieldValidationError>* errors,
    const std::string& path);

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
    if (field.file && field.kind != ConfigValueKind::kString) {
      if (error) *error = "File parameters must be strings: " + field.name;
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
        case ConfigValueKind::kMap:
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
        case ConfigValueKind::kJson:
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
    if (field.items) {
      if (field.kind != ConfigValueKind::kArray &&
          field.kind != ConfigValueKind::kMap) {
        if (error)
          *error = "Items only allowed for Array or Map: " + field.name;
        return false;
      }
      auto item = *field.items;
      item.name = field.name + " item";
      if (!ValidateConfigFieldDefinitions({item}, error)) return false;
    }
    if (field.fields) {
      if (field.kind != ConfigValueKind::kObject) {
        if (error) *error = "Fields only allowed for Object: " + field.name;
        return false;
      }
      if (!ValidateConfigFieldDefinitions(*field.fields, error)) return false;
    }
    if (!field.default_value.is_null() && (field.items || field.fields)) {
      nlohmann::json normalized;
      std::vector<ConfigFieldValidationError> errors;
      if (!detail::NormalizeConfigValue(field, field.default_value, &normalized,
                                        &errors, "")) {
        if (error)
          *error = "Invalid default for " + field.name + ": " +
                   errors.front().message;
        return false;
      }
    }
  }
  return true;
}

namespace detail {

inline std::string ConfigPointerToken(const std::string& value) {
  std::string escaped;
  for (char c : value) {
    if (c == '~')
      escaped += "~0";
    else if (c == '/')
      escaped += "~1";
    else
      escaped += c;
  }
  return escaped;
}

inline bool ConfigError(std::vector<ConfigFieldValidationError>* errors,
                        const std::string& path, ConfigFieldErrorKind kind,
                        std::string message) {
  if (errors) errors->push_back({path, kind, std::move(message)});
  return false;
}

inline bool NormalizeConfigObject(
    const std::vector<ConfigFieldDefinition>& fields,
    const nlohmann::json& input, nlohmann::json* normalized,
    std::vector<ConfigFieldValidationError>* errors, const std::string& path) {
  if (!input.is_object())
    return ConfigError(errors, path, ConfigFieldErrorKind::kNotAnObject,
                       "Config must be a JSON object");
  bool ok = true;
  auto result = nlohmann::json::object();
  for (auto it = input.begin(); it != input.end(); ++it) {
    const auto found =
        std::find_if(fields.begin(), fields.end(),
                     [&](const auto& f) { return f.name == it.key(); });
    if (found == fields.end()) {
      ConfigError(errors, path + "/" + ConfigPointerToken(it.key()),
                  ConfigFieldErrorKind::kUnknownField,
                  "Unknown config field: " + it.key());
      ok = false;
    }
  }
  for (const auto& field : fields) {
    const auto field_path = path + "/" + ConfigPointerToken(field.name);
    const auto entry = input.find(field.name);
    if (entry == input.end() && field.required) {
      ConfigError(errors, field_path, ConfigFieldErrorKind::kMissingField,
                  "Missing required config field: " + field.name);
      ok = false;
      continue;
    }
    if (entry == input.end() && field.default_value.is_null()) continue;
    const auto& value = entry == input.end() ? field.default_value : *entry;
    nlohmann::json next;
    if (!NormalizeConfigValue(field, value, &next, errors, field_path)) {
      ok = false;
      continue;
    }
    result[field.name] = std::move(next);
  }
  if (ok && normalized) *normalized = std::move(result);
  return ok;
}

inline bool NormalizeConfigValue(
    const ConfigFieldDefinition& field, const nlohmann::json& value,
    nlohmann::json* normalized, std::vector<ConfigFieldValidationError>* errors,
    const std::string& path) {
  if (value.is_number() && !std::isfinite(value.get<double>()))
    return ConfigError(errors, path, ConfigFieldErrorKind::kNonFinite,
                       "Numeric value must be finite for field: " + field.name);
  bool matches = false;
  switch (field.kind) {
    case ConfigValueKind::kString:
      matches = value.is_string();
      break;
    case ConfigValueKind::kInteger:
      matches = value.is_number_integer() || value.is_number_unsigned();
      break;
    case ConfigValueKind::kNumber:
      matches = value.is_number();
      break;
    case ConfigValueKind::kBoolean:
      matches = value.is_boolean();
      break;
    case ConfigValueKind::kMap:
    case ConfigValueKind::kObject:
      matches = value.is_object();
      break;
    case ConfigValueKind::kArray:
      matches = value.is_array();
      break;
    case ConfigValueKind::kJson:
      matches = !value.is_null();
      break;
  }
  if (!matches)
    return ConfigError(
        errors, path, ConfigFieldErrorKind::kTypeMismatch,
        "Expected " + std::string(ConfigValueKindName(field.kind)));
  if (value.is_number()) {
    if (field.minimum && IsValueBelowMinimum(value, *field.minimum))
      return ConfigError(
          errors, path, ConfigFieldErrorKind::kOutOfRange,
          "Numeric value is below minimum " + std::to_string(*field.minimum));
    if (field.maximum && IsValueAboveMaximum(value, *field.maximum))
      return ConfigError(
          errors, path, ConfigFieldErrorKind::kOutOfRange,
          "Numeric value exceeds maximum " + std::to_string(*field.maximum));
  }
  if (!field.enum_values.empty() && value.is_string()) {
    const auto& text = value.get_ref<const std::string&>();
    if (std::find(field.enum_values.begin(), field.enum_values.end(), text) ==
        field.enum_values.end())
      return ConfigError(
          errors, path, ConfigFieldErrorKind::kInvalidEnum,
          "String value '" + text + "' not in allowed enum values");
  }
  if (field.fields)
    return NormalizeConfigObject(*field.fields, value, normalized, errors,
                                 path);
  if (field.items) {
    auto result = field.kind == ConfigValueKind::kArray
                      ? nlohmann::json::array()
                      : nlohmann::json::object();
    bool ok = true;
    for (auto it = value.begin(); it != value.end(); ++it) {
      const auto key = value.is_array() ? std::to_string(it - value.begin())
                                        : ConfigPointerToken(it.key());
      nlohmann::json next;
      if (!NormalizeConfigValue(*field.items, *it, &next, errors,
                                path + "/" + key)) {
        ok = false;
        continue;
      }
      if (value.is_array())
        result.push_back(std::move(next));
      else
        result[it.key()] = std::move(next);
    }
    if (ok && normalized) *normalized = std::move(result);
    return ok;
  }
  if (normalized) *normalized = value;
  return true;
}

}  // namespace detail

inline bool ValidateAndNormalizeFields(
    const std::vector<ConfigFieldDefinition>& schema,
    const nlohmann::json& input, nlohmann::json* normalized,
    std::vector<ConfigFieldValidationError>* errors) {
  return detail::NormalizeConfigObject(schema, input, normalized, errors, "");
}

}  // namespace llm_edgeflow

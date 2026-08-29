#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

#include "contracts/config_schema.h"

namespace alg_framework {

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
          const long double value =
              field.default_value.is_number_unsigned()
                  ? static_cast<long double>(
                        field.default_value.get<uint64_t>())
                  : static_cast<long double>(
                        field.default_value.get<int64_t>());
          if ((field.minimum.has_value() && value < *field.minimum) ||
              (field.maximum.has_value() && value > *field.maximum)) {
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

}  // namespace alg_framework

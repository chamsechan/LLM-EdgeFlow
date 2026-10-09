#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace llm_edgeflow {

/**
 * @brief 配置字段取值类型枚举
 */
enum class ConfigValueKind {
  kString,
  kInteger,
  kNumber,
  kBoolean,
  kObject,
  kArray,
};

/**
 * @brief 结构化配置字段模式定义 (Schema Definition)
 */
struct ConfigFieldDefinition {
  std::string name;
  ConfigValueKind kind = ConfigValueKind::kString;
  bool required = false;
  nlohmann::json default_value;
  std::optional<double> minimum;
  std::optional<double> maximum;
  std::vector<std::string> enum_values;
  std::string semantic;

  ConfigFieldDefinition() = default;
  ConfigFieldDefinition(std::string field_name, ConfigValueKind value_kind,
                        bool is_required = false,
                        nlohmann::json field_default = nlohmann::json(),
                        std::optional<double> field_minimum = std::nullopt,
                        std::optional<double> field_maximum = std::nullopt,
                        std::vector<std::string> allowed_values = {},
                        std::string field_semantic = {})
      : name(std::move(field_name)),
        kind(value_kind),
        required(is_required),
        default_value(std::move(field_default)),
        minimum(field_minimum),
        maximum(field_maximum),
        enum_values(std::move(allowed_values)),
        semantic(std::move(field_semantic)) {}
};

inline const char* ConfigValueKindName(ConfigValueKind kind) noexcept {
  switch (kind) {
    case ConfigValueKind::kString:
      return "string";
    case ConfigValueKind::kInteger:
      return "integer";
    case ConfigValueKind::kNumber:
      return "number";
    case ConfigValueKind::kBoolean:
      return "boolean";
    case ConfigValueKind::kObject:
      return "object";
    case ConfigValueKind::kArray:
      return "array";
    default:
      return "unknown";
  }
}

inline nlohmann::json ConfigFieldToJson(const ConfigFieldDefinition& field) {
  nlohmann::json result = {{"name", field.name},
                           {"type", ConfigValueKindName(field.kind)},
                           {"required", field.required}};
  if (!field.default_value.is_null()) result["default"] = field.default_value;
  if (field.minimum) result["minimum"] = *field.minimum;
  if (field.maximum) result["maximum"] = *field.maximum;
  if (!field.enum_values.empty()) result["enum"] = field.enum_values;
  if (!field.semantic.empty()) result["semantic"] = field.semantic;
  return result;
}

// 读取已声明的配置字段：有配置值时取配置值，否则取声明中的默认值，
// 因此每个默认值只在 Definition 中写一次。字段未声明或配置值类型错误时
// 抛出异常，与 nlohmann::json::value 一致。null 表示未配置，
// 其他非对象值会被拒绝。
template <typename T>
T ConfigValueOrDefault(const nlohmann::json& config,
                       const std::vector<ConfigFieldDefinition>& fields,
                       const std::string& name) {
  if (!config.is_object() && !config.is_null()) {
    throw std::invalid_argument("Configuration must be an object or null");
  }
  for (const auto& field : fields) {
    if (field.name != name) continue;
    if (config.is_object()) {
      const auto it = config.find(name);
      if (it != config.end()) return it->template get<T>();
    }
    return field.default_value.template get<T>();
  }
  throw std::invalid_argument("Undeclared configuration field: " + name);
}

}  // namespace llm_edgeflow

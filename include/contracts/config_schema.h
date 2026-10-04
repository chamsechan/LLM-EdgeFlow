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

// Reads a declared configuration field: the configured value when present,
// otherwise the default from its declaration, so each default is written once
// in the Definition. Throws for undeclared fields or a configured value of the
// wrong type, like nlohmann::json::value. Null means omitted configuration;
// other non-object values are rejected.
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

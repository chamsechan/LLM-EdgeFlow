#pragma once

#include <nlohmann/json.hpp>
#include <optional>
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

}  // namespace llm_edgeflow

#pragma once

#include <memory>
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
  kMap,
  kJson,
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
  std::shared_ptr<const ConfigFieldDefinition> items;
  std::optional<std::vector<ConfigFieldDefinition>> fields;
  bool file = false;

  ConfigFieldDefinition() = default;
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
    case ConfigValueKind::kMap:
      return "map";
    case ConfigValueKind::kJson:
      return "json";
    default:
      return "unknown";
  }
}

inline nlohmann::json ConfigFieldToJson(const ConfigFieldDefinition& field) {
  nlohmann::json result = {{"type", ConfigValueKindName(field.kind)},
                           {"required", field.required}};
  if (!field.name.empty()) result["name"] = field.name;
  if (!field.default_value.is_null()) result["default"] = field.default_value;
  if (field.minimum) result["minimum"] = *field.minimum;
  if (field.maximum) result["maximum"] = *field.maximum;
  if (!field.enum_values.empty()) result["enum"] = field.enum_values;
  if (!field.semantic.empty()) result["semantic"] = field.semantic;
  if (field.file) result["file"] = true;
  if (field.items) result["items"] = ConfigFieldToJson(*field.items);
  if (field.fields) {
    result["fields"] = nlohmann::json::array();
    for (const auto& nested : *field.fields) {
      result["fields"].push_back(ConfigFieldToJson(nested));
    }
  }
  return result;
}

inline nlohmann::json ConfigFieldJsonSchema(
    const ConfigFieldDefinition& field) {
  auto schema = nlohmann::json::object();
  if (field.kind == ConfigValueKind::kJson) {
    schema["not"] = {{"type", "null"}};
  } else {
    schema["type"] = field.kind == ConfigValueKind::kMap
                         ? "object"
                         : ConfigValueKindName(field.kind);
  }
  if (field.minimum) schema["minimum"] = *field.minimum;
  if (field.maximum) schema["maximum"] = *field.maximum;
  if (!field.enum_values.empty()) schema["enum"] = field.enum_values;
  if (!field.default_value.is_null()) schema["default"] = field.default_value;
  if (!field.semantic.empty()) schema["description"] = field.semantic;
  if (field.file) schema["file"] = true;
  if (field.items) {
    schema[field.kind == ConfigValueKind::kMap ? "additionalProperties"
                                               : "items"] =
        ConfigFieldJsonSchema(*field.items);
  }
  if (field.fields) {
    schema["properties"] = nlohmann::json::object();
    schema["required"] = nlohmann::json::array();
    schema["additionalProperties"] = false;
    for (const auto& nested : *field.fields) {
      schema["properties"][nested.name] = ConfigFieldJsonSchema(nested);
      if (nested.required) schema["required"].push_back(nested.name);
    }
  }
  return schema;
}

}  // namespace llm_edgeflow

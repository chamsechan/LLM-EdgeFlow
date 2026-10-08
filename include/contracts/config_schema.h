#pragma once

#include <memory>
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
  kObject,  // 只用于结构体元素：对象，只允许 fields 声明的键
  kArray,
  kMap,   // 对象，键由使用者自定，值按 items 声明
  kJson,  // 任意 JSON 值，不能为 null
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
  // 数组、映射的元素定义；name 为空，不设置时元素不受检查。
  std::shared_ptr<const ConfigFieldDefinition> items;
  // 结构体元素（kObject）的字段；为空时不检查对象内部。
  std::vector<ConfigFieldDefinition> fields;

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
    case ConfigValueKind::kMap:
      return "map";
    case ConfigValueKind::kJson:
      return "json";
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
  if (field.items) result["items"] = ConfigFieldToJson(*field.items);
  if (!field.fields.empty()) {
    nlohmann::json fields = nlohmann::json::array();
    for (const auto& item : field.fields)
      fields.push_back(ConfigFieldToJson(item));
    result["fields"] = std::move(fields);
  }
  return result;
}

// 由字段声明生成 JSON schema（控制命令的 payload 格式使用）。只用
// ValidateControlSchema 支持的关键字；json 类型不限制类型。
inline nlohmann::json ConfigFieldJsonSchema(
    const ConfigFieldDefinition& field) {
  nlohmann::json schema = nlohmann::json::object();
  switch (field.kind) {
    case ConfigValueKind::kString:
      schema["type"] = "string";
      if (!field.enum_values.empty()) schema["enum"] = field.enum_values;
      break;
    case ConfigValueKind::kBoolean:
      schema["type"] = "boolean";
      break;
    case ConfigValueKind::kInteger:
    case ConfigValueKind::kNumber:
      schema["type"] = ConfigValueKindName(field.kind);
      if (field.minimum) schema["minimum"] = *field.minimum;
      if (field.maximum) schema["maximum"] = *field.maximum;
      break;
    case ConfigValueKind::kArray:
      schema["type"] = "array";
      if (field.items) schema["items"] = ConfigFieldJsonSchema(*field.items);
      break;
    case ConfigValueKind::kMap:
      schema["type"] = "object";
      if (field.items) {
        schema["additionalProperties"] = ConfigFieldJsonSchema(*field.items);
      }
      break;
    case ConfigValueKind::kObject: {
      schema["type"] = "object";
      if (!field.fields.empty()) {
        schema["additionalProperties"] = false;
        nlohmann::json properties = nlohmann::json::object();
        nlohmann::json required = nlohmann::json::array();
        for (const auto& item : field.fields) {
          properties[item.name] = ConfigFieldJsonSchema(item);
          if (item.required) required.push_back(item.name);
        }
        schema["properties"] = std::move(properties);
        schema["required"] = std::move(required);
      }
      break;
    }
    case ConfigValueKind::kJson:
      break;
  }
  if (!field.semantic.empty()) schema["description"] = field.semantic;
  if (!field.default_value.is_null()) schema["default"] = field.default_value;
  return schema;
}

}  // namespace llm_edgeflow

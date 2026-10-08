#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "contracts/config_schema.h"

namespace llm_edgeflow {

// 测试中直接构造 Definition 字段列表的简写；生产代码用 Field 声明参数。
inline ConfigFieldDefinition MakeConfigField(
    std::string name, ConfigValueKind kind, bool required = false,
    nlohmann::json default_value = nlohmann::json(),
    std::optional<double> minimum = std::nullopt,
    std::optional<double> maximum = std::nullopt,
    std::vector<std::string> enum_values = {}, std::string semantic = {}) {
  ConfigFieldDefinition definition;
  definition.name = std::move(name);
  definition.kind = kind;
  definition.required = required;
  definition.default_value = std::move(default_value);
  definition.minimum = minimum;
  definition.maximum = maximum;
  definition.enum_values = std::move(enum_values);
  definition.semantic = std::move(semantic);
  return definition;
}

}  // namespace llm_edgeflow

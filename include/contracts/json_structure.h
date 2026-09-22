#pragma once

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace llm_edgeflow::json_structure {

// Shallow structural predicates, shared by the existing ordered parsers and
// their editor schema. No recursive validation, normalization or diagnostics.
using Json = nlohmann::json;

inline Json Object(Json properties, Json required = Json::array()) {
  return {{"type", "object"},
          {"properties", std::move(properties)},
          {"required", std::move(required)},
          {"additionalProperties", false}};
}

inline Json NonemptyString() { return {{"type", "string"}, {"minLength", 1}}; }

inline const Json& Property(const Json& shape, const char* name) {
  return shape.at("properties").at(name);
}

inline bool AllowsProperty(const Json& shape, const std::string& name) {
  return shape.at("properties").contains(name);
}

inline bool MissingRequired(const Json& object, const Json& shape,
                            const char* name) {
  const auto& required = shape.at("required");
  return !object.contains(name) &&
         std::find(required.begin(), required.end(), name) != required.end();
}

inline bool HasType(const Json& value, const Json& shape) {
  if (!shape.contains("type")) return true;
  const auto& type = shape.at("type").get_ref<const std::string&>();
  if (type == "string") return value.is_string();
  if (type == "object") return value.is_object();
  if (type == "array") return value.is_array();
  if (type == "integer") return value.is_number_integer();
  if (type == "number") return value.is_number();
  if (type == "boolean") return value.is_boolean();
  if (type == "null") return value.is_null();
  return false;
}

inline bool TooShort(const Json& value, const Json& shape) {
  const char* keyword = value.is_string() ? "minLength" : "minItems";
  const auto minimum = shape.find(keyword);
  const auto size = value.is_string()
                        ? value.get_ref<const std::string&>().size()
                        : value.size();
  return minimum != shape.end() && size < minimum->get<size_t>();
}

inline bool TooLong(const Json& value, const Json& shape) {
  const auto maximum = shape.find("maxItems");
  return maximum != shape.end() && value.size() > maximum->get<size_t>();
}

template <typename T>
inline bool BelowMinimum(T value, const Json& shape) {
  const auto minimum = shape.find("minimum");
  return minimum != shape.end() && value < minimum->get<T>();
}

template <typename T>
inline bool AboveMaximum(T value, const Json& shape) {
  const auto maximum = shape.find("maximum");
  return maximum != shape.end() && value > maximum->get<T>();
}

inline bool AllowsValue(const Json& value, const Json& shape) {
  const auto values = shape.find("enum");
  return values == shape.end() ||
         std::find(values->begin(), values->end(), value) != values->end();
}

}  // namespace llm_edgeflow::json_structure

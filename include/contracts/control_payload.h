#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>

#include "nlohmann/json.hpp"

namespace llm_edgeflow {

// Supported validation keywords: type, enum, required, properties,
// minProperties, additionalProperties, homogeneous items, minimum and maximum.
// Documentary annotations do not change validation or apply defaults. Reject
// unsupported or malformed declarations at registration and in this helper.
inline bool ValidateControlSchema(const nlohmann::json& schema,
                                  std::string* error = nullptr) {
  if (error) error->clear();
  const auto fail = [&](const std::string& message) {
    if (error) *error = message;
    return false;
  };
  if (!schema.is_object())
    return fail("Control payload schema must be an object");
  for (auto it = schema.begin(); it != schema.end(); ++it) {
    const auto& key = it.key();
    const auto& value = it.value();
    if (key == "type") {
      static const std::unordered_set<std::string> types = {
          "object", "array", "string", "boolean", "number", "integer", "null"};
      if (!value.is_string() || !types.count(value.get<std::string>()))
        return fail("Control schema type must name a supported JSON type");
    } else if (key == "minimum" || key == "maximum") {
      if (!value.is_number() || !std::isfinite(value.get<long double>()))
        return fail("Control schema " + key + " must be a finite number");
    } else if (key == "minProperties") {
      if (!value.is_number_integer() || value.get<long double>() < 0)
        return fail(
            "Control schema minProperties must be a nonnegative integer");
    } else if (key == "enum") {
      if (!value.is_array() || value.empty())
        return fail("Control schema enum must be a nonempty array");
      for (auto entry = value.begin(); entry != value.end(); ++entry) {
        if (std::find(value.begin(), entry, *entry) != entry)
          return fail("Control schema enum contains duplicate values");
      }
    } else if (key == "required") {
      if (!value.is_array())
        return fail(
            "Control schema required must be an array of unique strings");
      std::unordered_set<std::string> names;
      for (const auto& entry : value) {
        if (!entry.is_string() ||
            !names.insert(entry.get<std::string>()).second)
          return fail(
              "Control schema required must be an array of unique strings");
      }
    } else if (key == "properties") {
      if (!value.is_object())
        return fail("Control schema properties must be an object");
      for (auto property = value.begin(); property != value.end(); ++property) {
        std::string detail;
        if (!ValidateControlSchema(property.value(), &detail))
          return fail("Property '" + property.key() + "': " + detail);
      }
    } else if (key == "items" || key == "additionalProperties") {
      if (key == "additionalProperties" && value.is_boolean()) continue;
      std::string detail;
      if (!ValidateControlSchema(value, &detail))
        return fail("Control schema " + key + ": " + detail);
    } else if (key == "title" || key == "description" || key == "$comment") {
      if (!value.is_string())
        return fail("Control schema " + key + " annotation must be a string");
    } else if (key == "examples") {
      if (!value.is_array())
        return fail("Control schema examples annotation must be an array");
    } else if (key == "deprecated" || key == "readOnly" || key == "writeOnly") {
      if (!value.is_boolean())
        return fail("Control schema " + key + " annotation must be a boolean");
    } else if (key != "default") {
      return fail("Unsupported Control schema keyword: " + key);
    }
  }
  if (schema.contains("minimum") && schema.contains("maximum") &&
      schema["minimum"].get<long double>() >
          schema["maximum"].get<long double>())
    return fail("Control schema minimum cannot exceed maximum");
  return true;
}

namespace control_payload_detail {

inline bool ValidateValue(const nlohmann::json& payload,
                          const nlohmann::json& schema, std::string* err_msg) {
  const auto fail = [&](const std::string& message) {
    if (err_msg) *err_msg = message;
    return false;
  };
  if (schema.contains("type") && schema["type"].is_string()) {
    const std::string type = schema["type"].get<std::string>();
    const bool matches = (type == "object" && payload.is_object()) ||
                         (type == "array" && payload.is_array()) ||
                         (type == "string" && payload.is_string()) ||
                         (type == "boolean" && payload.is_boolean()) ||
                         (type == "number" && payload.is_number()) ||
                         (type == "integer" && payload.is_number_integer()) ||
                         (type == "null" && payload.is_null());
    if (!matches)
      return fail("Control payload does not match type '" + type + "'");
  }

  if (schema.contains("enum") && schema["enum"].is_array() &&
      std::find(schema["enum"].begin(), schema["enum"].end(), payload) ==
          schema["enum"].end()) {
    return fail("Control payload value is not in the allowed enum");
  }

  if (payload.is_number()) {
    const auto number = payload.get<long double>();
    if (!std::isfinite(number))
      return fail("Control payload number must be finite");
    if (schema.contains("minimum") &&
        number < schema["minimum"].get<long double>())
      return fail("Control payload number is below minimum " +
                  schema["minimum"].dump());
    if (schema.contains("maximum") &&
        number > schema["maximum"].get<long double>())
      return fail("Control payload number is above maximum " +
                  schema["maximum"].dump());
  }

  if (payload.is_object()) {
    if (schema.contains("minProperties") &&
        schema["minProperties"].is_number_integer()) {
      const auto minimum = schema["minProperties"].get<uint64_t>();
      if (payload.size() < minimum) {
        return fail("Control payload has fewer properties than required");
      }
    }

    if (schema.contains("required") && schema["required"].is_array()) {
      for (const auto& req : schema["required"]) {
        if (req.is_string()) {
          std::string req_key = req.get<std::string>();
          if (!payload.contains(req_key)) {
            return fail("Missing required field in control payload: " +
                        req_key);
          }
        }
      }
    }

    const nlohmann::json empty_properties = nlohmann::json::object();
    const auto& properties =
        schema.contains("properties") && schema["properties"].is_object()
            ? schema["properties"]
            : empty_properties;
    for (auto it = payload.begin(); it != payload.end(); ++it) {
      if (properties.contains(it.key())) {
        std::string nested_error;
        if (!ValidateValue(it.value(), properties[it.key()], &nested_error)) {
          return fail("Property '" + it.key() + "': " + nested_error);
        }
      } else if (schema.contains("additionalProperties")) {
        const auto& additional = schema["additionalProperties"];
        if (additional.is_boolean() && !additional.get<bool>()) {
          return fail("Unknown property in control payload: " + it.key());
        }
        if (additional.is_object()) {
          std::string nested_error;
          if (!ValidateValue(it.value(), additional, &nested_error)) {
            return fail("Property '" + it.key() + "': " + nested_error);
          }
        }
      }
    }
  }

  if (payload.is_array() && schema.contains("items") &&
      schema["items"].is_object()) {
    for (size_t i = 0; i < payload.size(); ++i) {
      std::string nested_error;
      if (!ValidateValue(payload[i], schema["items"], &nested_error)) {
        return fail("Array item " + std::to_string(i) + ": " + nested_error);
      }
    }
  }
  return true;
}

}  // namespace control_payload_detail

// Structural validation only; Nodes still validate domain semantics.
inline bool ValidateControlPayload(const nlohmann::json& payload,
                                   const nlohmann::json& schema,
                                   std::string* error = nullptr) {
  if (!ValidateControlSchema(schema, error)) return false;
  return control_payload_detail::ValidateValue(payload, schema, error);
}

// Owns the parsed payload; no borrowed platform buffers survive this call.
// The output is replaced only after parsing and structural validation succeed.
inline bool ParseControlPayload(const std::string& text,
                                const nlohmann::json& schema,
                                nlohmann::json* payload,
                                std::string* error = nullptr) {
  if (error) error->clear();
  if (!payload) {
    if (error) *error = "Null control payload output";
    return false;
  }
  try {
    auto parsed = nlohmann::json::parse(text);
    if (!ValidateControlPayload(parsed, schema, error)) return false;
    *payload = std::move(parsed);
    return true;
  } catch (const nlohmann::json::exception& e) {
    if (error) *error = std::string("Invalid control JSON: ") + e.what();
    return false;
  }
}

}  // namespace llm_edgeflow

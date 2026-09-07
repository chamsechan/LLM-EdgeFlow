#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include "nlohmann/json.hpp"

namespace llm_edgeflow {

// Supported schema subset: type, enum, required, properties, minProperties,
// additionalProperties and homogeneous items. Domain validation stays in Nodes.
inline bool ValidateControlPayload(const nlohmann::json& payload,
                                   const nlohmann::json& schema,
                                   std::string* err_msg = nullptr) {
  if (err_msg) err_msg->clear();

  const auto fail = [&](const std::string& message) {
    if (err_msg) *err_msg = message;
    return false;
  };
  if (!schema.is_object())
    return fail("Control payload schema must be an object");
  if (schema.empty()) return true;

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

  if (payload.is_object()) {
    if (schema.contains("minProperties") &&
        schema["minProperties"].is_number_integer()) {
      const auto minimum = schema["minProperties"].get<int64_t>();
      if (minimum >= 0 && payload.size() < static_cast<size_t>(minimum)) {
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
        if (!ValidateControlPayload(it.value(), properties[it.key()],
                                    &nested_error)) {
          return fail("Property '" + it.key() + "': " + nested_error);
        }
      } else if (schema.contains("additionalProperties")) {
        const auto& additional = schema["additionalProperties"];
        if (additional.is_boolean() && !additional.get<bool>()) {
          return fail("Unknown property in control payload: " + it.key());
        }
        if (additional.is_object()) {
          std::string nested_error;
          if (!ValidateControlPayload(it.value(), additional, &nested_error)) {
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
      if (!ValidateControlPayload(payload[i], schema["items"], &nested_error)) {
        return fail("Array item " + std::to_string(i) + ": " + nested_error);
      }
    }
  }
  return true;
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

#include "adapter/pipeline_document.h"

#include "adapter/io_structure.h"
#include "contracts/json_structure.h"

namespace llm_edgeflow {

namespace structure = json_structure;

namespace {

bool Fail(const std::string& path, const std::string& message,
          std::string* out_error, std::string* out_error_path) {
  if (out_error) *out_error = message;
  if (out_error_path) *out_error_path = path;
  return false;
}

bool ParseItems(const nlohmann::json& io, const char* side,
                std::vector<IoItemSpec>* items, std::string* out_error,
                std::string* out_error_path) {
  const auto& io_shape = IoStructure();
  const auto& array_shape = structure::Property(io_shape, side);
  const auto& item_shape = array_shape.at("items");
  const std::string base = std::string("/io/") + side;
  if (!io.contains(side)) {
    return Fail(base, "Missing required field '" + base + "'", out_error,
                out_error_path);
  }
  const auto& array = io.at(side);
  if (!structure::HasType(array, array_shape)) {
    return Fail(base, "Field '" + base + "' must be an array", out_error,
                out_error_path);
  }
  if (structure::TooShort(array, array_shape)) {
    return Fail(base, "Field '" + base + "' must contain at least one item",
                out_error, out_error_path);
  }
  for (size_t i = 0; i < array.size(); ++i) {
    const auto& entry = array[i];
    const std::string path = base + "/" + std::to_string(i);
    if (!structure::HasType(entry, item_shape)) {
      return Fail(path, "Field '" + path + "' must be an object", out_error,
                  out_error_path);
    }
    for (auto it = entry.begin(); it != entry.end(); ++it) {
      if (!structure::AllowsProperty(item_shape, it.key())) {
        return Fail(path + "/" + EscapeJsonPointer(it.key()),
                    "Unknown field at " + path + "/" +
                        EscapeJsonPointer(it.key()) +
                        " (only 'type', 'name' and 'params' allowed)",
                    out_error, out_error_path);
      }
    }
    IoItemSpec item;
    for (const char* field : {"type", "name"}) {
      const std::string field_path = path + "/" + field;
      if (!entry.contains(field)) {
        return Fail(field_path, "Missing required field '" + field_path + "'",
                    out_error, out_error_path);
      }
      const auto& value = entry.at(field);
      const auto& field_shape = structure::Property(item_shape, field);
      if (!structure::HasType(value, field_shape)) {
        return Fail(field_path, "Field '" + field_path + "' must be a string",
                    out_error, out_error_path);
      }
      if (structure::TooShort(value, field_shape)) {
        return Fail(field_path, "Field '" + field_path + "' cannot be empty",
                    out_error, out_error_path);
      }
      (std::string(field) == "type" ? item.type : item.name) =
          value.get<std::string>();
    }
    if (entry.contains("params")) {
      if (!structure::HasType(entry.at("params"),
                              structure::Property(item_shape, "params"))) {
        return Fail(path + "/params",
                    "Field '" + path + "/params' must be an object", out_error,
                    out_error_path);
      }
      item.params = entry.at("params");
    }
    items->push_back(std::move(item));
  }
  return true;
}

}  // namespace

bool SplitPipelineDocument(const nlohmann::json& root,
                           PipelineDocumentSplit* out_split,
                           std::string* out_error,
                           std::string* out_error_path) {
  if (!out_split) {
    if (out_error) *out_error = "Null out_split pointer";
    return false;
  }
  *out_split = PipelineDocumentSplit{};

  if (!root.is_object()) {
    return Fail("/", "Pipeline configuration root must be a JSON object",
                out_error, out_error_path);
  }
  for (const auto& [field, value] : root.items()) {
    if (!structure::AllowsProperty(PipelineDocumentStructure(), field)) {
      const auto path = "/" + EscapeJsonPointer(field);
      return Fail(path, "Unknown field at " + path, out_error, out_error_path);
    }
  }
  if (!root.contains("io")) {
    return Fail("/io", "Missing required field '/io'", out_error,
                out_error_path);
  }
  const auto& io = root.at("io");
  const auto& io_shape = IoStructure();
  if (!structure::HasType(io, io_shape)) {
    return Fail("/io", "Field '/io' must be an object", out_error,
                out_error_path);
  }
  for (auto it = io.begin(); it != io.end(); ++it) {
    if (!structure::AllowsProperty(io_shape, it.key())) {
      return Fail("/io/" + EscapeJsonPointer(it.key()),
                  "Unknown field at /io/" + EscapeJsonPointer(it.key()) +
                      " (only 'input' and 'output' allowed)",
                  out_error, out_error_path);
    }
  }
  PipelineDocumentSplit split;
  if (!ParseItems(io, "input", &split.io.input, out_error, out_error_path) ||
      !ParseItems(io, "output", &split.io.output, out_error, out_error_path)) {
    return false;
  }
  split.core_json = root;
  split.core_json.erase("io");
  *out_split = std::move(split);
  return true;
}

}  // namespace llm_edgeflow

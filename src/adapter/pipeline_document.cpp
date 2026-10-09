#include "adapter/pipeline_document.h"

#include "adapter/io_structure.h"
#include "contracts/json_structure.h"

namespace llm_edgeflow {
namespace {
bool CheckObject(const nlohmann::json& value, const nlohmann::json& shape,
                 const std::string& path, std::string* error,
                 std::string* error_path) {
  const auto fail = [&](const std::string& at, const std::string& message) {
    if (error) *error = message + " at " + at;
    if (error_path) *error_path = at;
    return false;
  };
  if (!value.is_object()) return fail(path, "Expected an object");
  for (const auto& [field, ignored] : value.items()) {
    if (!json_structure::AllowsProperty(shape, field))
      return fail(path == "/" ? "/" + EscapeJsonPointer(field)
                              : path + "/" + EscapeJsonPointer(field),
                  "Unknown field");
  }
  for (const auto& field : shape.value("required", nlohmann::json::array())) {
    if (!value.contains(field.get<std::string>()))
      return fail(path == "/" ? "/" + field.get<std::string>()
                              : path + "/" + field.get<std::string>(),
                  "Missing required field");
  }
  return true;
}
}  // namespace

bool SplitPipelineDocument(const nlohmann::json& root,
                           PipelineDocumentSplit* out_split,
                           std::string* out_error,
                           std::string* out_error_path) {
  if (out_error) out_error->clear();
  if (out_error_path) out_error_path->clear();
  if (!out_split) {
    if (out_error) *out_error = "Null out_split pointer";
    return false;
  }
  *out_split = {};
  const auto fail = [&](const std::string& path, const std::string& message) {
    if (out_error) *out_error = message + " at " + path;
    if (out_error_path) *out_error_path = path;
    return false;
  };
  if (!CheckObject(root, PipelineDocumentStructure(), "/", out_error,
                   out_error_path) ||
      !CheckObject(root.at("io"), IoStructure(), "/io", out_error,
                   out_error_path))
    return false;
  PipelineDocumentSplit result;
  for (const auto* side : {"input", "output"}) {
    const auto path = std::string("/io/") + side;
    const auto& entries = root["io"][side];
    if (!entries.is_array() || entries.empty())
      return fail(path, "Expected a nonempty array");
    auto& destination =
        std::string(side) == "input" ? result.inputs : result.outputs;
    for (size_t i = 0; i < entries.size(); ++i) {
      const auto entry_path = path + "/" + std::to_string(i);
      const auto& entry = entries[i];
      if (!CheckObject(entry, IoEntryStructure(), entry_path, out_error,
                       out_error_path))
        return false;
      for (const auto* field : {"type", "name"}) {
        if (!entry[field].is_string() ||
            entry[field].get_ref<const std::string&>().empty())
          return fail(entry_path + "/" + field, "Expected a nonempty string");
      }
      if (entry.contains("params") && !entry["params"].is_object())
        return fail(entry_path + "/params", "Expected an object");
      destination.push_back({entry["type"].get<std::string>(),
                             entry["name"].get<std::string>(),
                             entry.value("params", nlohmann::json::object())});
    }
  }
  result.neutral_pipeline_json = root;
  result.neutral_pipeline_json.erase("io");
  *out_split = std::move(result);
  return true;
}
}  // namespace llm_edgeflow

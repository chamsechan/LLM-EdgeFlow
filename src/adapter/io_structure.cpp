#include "adapter/io_structure.h"

#include "contracts/json_structure.h"
#include "core/pipeline_config_structure.h"

namespace llm_edgeflow {
const nlohmann::json& IoEntryStructure(bool output) {
  static const auto input_shape =
      json_structure::Object({{"type", json_structure::NonemptyString()},
                              {"name", json_structure::NonemptyString()},
                              {"params", {{"type", "object"}}}},
                             {"type", "name"});
  static const auto output_shape = [] {
    auto shape = input_shape;
    shape["properties"]["inputs"] = {
        {"type", "object"}, {"additionalProperties", {{"type", "string"}}}};
    return shape;
  }();
  return output ? output_shape : input_shape;
}

const nlohmann::json& IoStructure() {
  static const auto shape = json_structure::Object(
      {{"input",
        {{"type", "array"}, {"minItems", 1}, {"items", IoEntryStructure()}}},
       {"output",
        {{"type", "array"},
         {"minItems", 1},
         {"items", IoEntryStructure(true)}}}},
      {"input", "output"});
  return shape;
}

const nlohmann::json& PipelineDocumentStructure() {
  static const auto shape = [] {
    auto result = PipelineConfigStructure();
    result["properties"]["io"] = IoStructure();
    result["required"].push_back("io");
    return result;
  }();
  return shape;
}
}  // namespace llm_edgeflow

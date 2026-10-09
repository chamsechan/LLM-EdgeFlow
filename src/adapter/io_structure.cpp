#include "adapter/io_structure.h"

#include "contracts/json_structure.h"
#include "core/pipeline_config_structure.h"

namespace llm_edgeflow {
const nlohmann::json& IoEntryStructure() {
  static const auto shape =
      json_structure::Object({{"type", json_structure::NonemptyString()},
                              {"name", json_structure::NonemptyString()},
                              {"params", {{"type", "object"}}}},
                             {"type", "name"});
  return shape;
}

const nlohmann::json& IoStructure() {
  static const auto shape = json_structure::Object(
      {{"input",
        {{"type", "array"}, {"minItems", 1}, {"items", IoEntryStructure()}}},
       {"output",
        {{"type", "array"}, {"minItems", 1}, {"items", IoEntryStructure()}}}},
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

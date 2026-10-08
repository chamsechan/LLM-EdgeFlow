#include "io_structure.h"

#include "contracts/json_structure.h"
#include "core/pipeline_config_structure.h"

namespace llm_edgeflow {
namespace {
using json_structure::Json;
using json_structure::NonemptyString;
using json_structure::Object;

Json IoItemArray() {
  return {{"type", "array"},
          {"minItems", 1},
          {"items", Object({{"type", NonemptyString()},
                            {"name", NonemptyString()},
                            {"params", {{"type", "object"}}}},
                           {"type", "name"})}};
}
}  // namespace

const nlohmann::json& IoStructure() {
  static const Json shape =
      Object({{"input", IoItemArray()}, {"output", IoItemArray()}},
             {"input", "output"});
  return shape;
}

const nlohmann::json& PipelineDocumentStructure() {
  static const Json shape = [] {
    auto result = PipelineConfigStructure();
    result["properties"]["io"] = IoStructure();
    result["required"].push_back("io");
    return result;
  }();
  return shape;
}

}  // namespace llm_edgeflow

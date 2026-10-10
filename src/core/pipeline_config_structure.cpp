#include "pipeline_config_structure.h"

#include "contracts/json_structure.h"

namespace llm_edgeflow {
namespace {
using json_structure::Json;
using json_structure::NonemptyString;
using json_structure::Object;
}  // namespace

const nlohmann::json& PipelineConfigStructure() {
  static const Json shape = [] {
    const Json mapping = {{"type", "object"},
                          {"additionalProperties", NonemptyString()}};
    auto node = Object({{"name", NonemptyString()},
                        {"type", NonemptyString()},
                        {"depends_on",
                         {{"type", "array"},
                          {"items", NonemptyString()},
                          {"maxItems", 256},
                          {"uniqueItems", true}}},
                        {"params", {{"type", "object"}}},
                        {"inputs", mapping}},
                       {"type", "name"});
    auto backend =
        Object({{"type", NonemptyString()}, {"params", {{"type", "object"}}}},
               {"type"});
    auto model = Object({{"type", NonemptyString()},
                         {"name", NonemptyString()},
                         {"file", NonemptyString()},
                         {"params", {{"type", "object"}}},
                         {"backend", std::move(backend)}},
                        {"type", "name", "file", "backend"});
    auto result = Object(
        {{"comment", {{"type", "string"}}},
         {"max_parallel_workers",
          {{"type", "integer"},
           {"minimum", 1},
           {"maximum", 64},
           {"default", 1}}},
         {"models",
          {{"type", "array"}, {"maxItems", 64}, {"items", std::move(model)}}},
         {"pipeline",
          {{"type", "array"},
           {"minItems", 1},
           {"maxItems", 256},
           {"items", std::move(node)}}}},
        {"pipeline"});
    return result;
  }();
  return shape;
}

}  // namespace llm_edgeflow

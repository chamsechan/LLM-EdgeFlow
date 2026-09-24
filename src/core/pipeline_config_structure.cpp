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
    auto node = Object({{"id", NonemptyString()},
                        {"node_type", NonemptyString()},
                        {"depends_on",
                         {{"type", "array"},
                          {"items", NonemptyString()},
                          {"maxItems", 256},
                          {"uniqueItems", true}}},
                        {"comment", {{"type", "string"}}},
                        {"config", {{"type", "object"}}},
                        {"inputs", mapping},
                        {"outputs", mapping}},
                       {"id", "node_type"});
    auto model = Object({{"model_id", NonemptyString()},
                         {"model_path", NonemptyString()},
                         {"model_type", NonemptyString()},
                         {"backend", NonemptyString()},
                         {"model_config", {{"type", "object"}}},
                         {"backend_config", {{"type", "object"}}},
                         {"comment", {{"type", "string"}}}},
                        {"model_id", "model_type", "backend", "model_path"});
    auto result = Object(
        {{"biz_name", NonemptyString()},
         {"comment", {{"type", "string"}}},
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
        {"biz_name", "pipeline"});
    return result;
  }();
  return shape;
}

}  // namespace llm_edgeflow

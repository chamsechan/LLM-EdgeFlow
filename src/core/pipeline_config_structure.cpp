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
    auto node =
        Object({{"id", NonemptyString()},
                {"node_type", NonemptyString()},
                {"depends_on",
                 {{"type", "array"},
                  {"items", NonemptyString()},
                  {"maxItems", 256},
                  {"uniqueItems", true}}},
                {"comment", {{"type", "string"}}},
                {"config", {{"type", "object"}}},
                {"ports", Object({{"inputs", mapping},
                                  {"outputs", mapping},
                                  // Existing parser accepts any ports.comment.
                                  {"comment", Json::object()}})}},
               {"id", "node_type", "depends_on"});
    auto model = Object(
        {{"model_id", NonemptyString()},
         {"model_path", NonemptyString()},
         {"model_type", NonemptyString()},
         {"capability", NonemptyString()},
         {"backend", NonemptyString()},
         {"model_config", {{"type", "object"}}},
         {"backend_config", {{"type", "object"}}},
         {"comment", {{"type", "string"}}}},
        {"model_id", "model_type", "capability", "backend", "model_path"});
    auto result = Object(
        {{"biz_name", NonemptyString()},
         {"comment", {{"type", "string"}}},
         {"execution_mode",
          {{"type", "string"},
           {"enum", {"sequential", "parallel"}},
           {"default", "sequential"}}},
         {"max_parallel_workers",
          {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}},
         {"models",
          {{"type", "array"}, {"maxItems", 64}, {"items", std::move(model)}}},
         {"pipeline",
          {{"type", "array"},
           {"minItems", 1},
           {"maxItems", 256},
           {"items", std::move(node)}}}},
        {"biz_name", "pipeline"});
    result["if"] = {{"required", {"max_parallel_workers"}}};
    result["then"] = {
        {"required", {"execution_mode"}},
        {"properties", {{"execution_mode", {{"const", "parallel"}}}}}};
    return result;
  }();
  return shape;
}

bool AllowsParallelWorkers(std::string_view execution_mode) {
  const auto& required_mode = PipelineConfigStructure()
                                  .at("then")
                                  .at("properties")
                                  .at("execution_mode")
                                  .at("const")
                                  .get_ref<const std::string&>();
  return execution_mode == required_mode;
}

}  // namespace llm_edgeflow

#include "deployment_structure.h"

#include <algorithm>

#include "contracts/json_structure.h"
#include "core/pipeline_config_structure.h"

namespace llm_edgeflow {
namespace {
using json_structure::Json;
using json_structure::NonemptyString;
using json_structure::Object;
}  // namespace

const nlohmann::json& PipelineDocumentStructure() {
  static const Json shape = [] {
    auto result = PipelineConfigStructure();
    result["properties"].erase("biz_name");
    auto& required = result["required"];
    required.erase(std::remove(required.begin(), required.end(), "biz_name"),
                   required.end());
    result["properties"]["deployment"] = DeploymentStructure();
    required.push_back("deployment");
    return result;
  }();
  return shape;
}

const nlohmann::json& OutputAllocationStructure() {
  static const Json shape = Object(
      {{"allocator", NonemptyString()},
       {"params",
        {{"description",
          "Allocator-defined JSON parameters; validate with the selected "
          "allocator."}}},
       {"meta_num",
        {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295ULL}}},
       {"metadata_type_id",
        {{"type", "integer"},
         {"minimum", -2147483648LL},
         {"maximum", 2147483647}}},
       {"capacities",
        {{"type", "object"},
         {"additionalProperties",
          {{"type", "integer"},
           {"minimum", 1},
           {"maximum", 4294967295ULL}}}}}});
  return shape;
}

const nlohmann::json& DeploymentStructure() {
  static const Json shape = Object(
      {{"io",
        Object({{"io_binding", NonemptyString()},
                {"out_mem",
                 {{"type", "object"},
                  {"additionalProperties", OutputAllocationStructure()}}}},
               {"io_binding"})}},
      {"io"});
  return shape;
}

}  // namespace llm_edgeflow

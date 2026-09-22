#include "deployment_structure.h"

#include "contracts/json_structure.h"

namespace llm_edgeflow {
namespace {
using json_structure::Json;
using json_structure::NonemptyString;
using json_structure::Object;
}  // namespace

const nlohmann::json& OutputAllocationStructure() {
  static const Json shape = Object(
      {{"type", NonemptyString()},
       {"allocator", NonemptyString()},
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
          {{"type", "integer"}, {"minimum", 1}, {"maximum", 4294967295ULL}}}}}},
      {"type"});
  return shape;
}

const nlohmann::json& DeploymentStructure() {
  static const Json shape = Object(
      {{"model_paths",
        {{"type", "object"},
         {"propertyNames", NonemptyString()},
         {"additionalProperties", NonemptyString()}}},
       {"io",
        Object({{"io_binding", NonemptyString()},
                {"output_allocations",
                 {{"type", "object"},
                  {"additionalProperties", OutputAllocationStructure()}}}},
               {"io_binding", "output_allocations"})}},
      {"io"});
  return shape;
}

}  // namespace llm_edgeflow

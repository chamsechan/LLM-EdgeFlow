#include "pipeline_json_schema.h"

#include <set>
#include <string>

#include "adapter/io_converter_registry.h"
#include "adapter/io_structure.h"
#include "contracts/config_schema.h"
#include "contracts/json_structure.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_config_structure.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {
namespace {
using Json = nlohmann::json;
using json_structure::Object;
using json_structure::Property;

Json Fields(const std::vector<ConfigFieldDefinition>& fields) {
  auto properties = Json::object();
  auto required = Json::array();
  for (const auto& field : fields) {
    properties[field.name] = ConfigFieldJsonSchema(field);
    if (field.required) required.push_back(field.name);
  }
  return Object(std::move(properties), std::move(required));
}

Json SourceReference() {
  return {{"type", "string"}, {"pattern", "^[^.]+\\.[^.]+$"}};
}

template <typename Ports>
Json Inputs(const Ports& ports) {
  auto properties = Json::object();
  auto required = Json::array();
  for (const auto& port : ports) {
    auto source = SourceReference();
    source["description"] = port.type_id;
    properties[port.logical_name] = std::move(source);
    if (port.required) required.push_back(port.logical_name);
  }
  return Object(std::move(properties), std::move(required));
}

void RequireParameters(Json* schema, const Json& params) {
  if (!params.at("required").empty()) (*schema)["required"].push_back("params");
}

Json SelectedType(const std::string& type) {
  return {{"properties", {{"type", {{"const", type}}}}},
          {"required", {"type"}}};
}

Json Nodes() {
  auto node = Property(PipelineConfigStructure(), "pipeline").at("items");
  auto types = Json::array();
  auto branches = Json::array();
  for (const auto& definition : PipelineCatalog::Nodes()) {
    types.push_back(definition.node_type);
    const auto params = Fields(definition.config_fields);
    const auto inputs = Inputs(definition.inputs);
    auto body = Json{{"properties", {{"params", params}, {"inputs", inputs}}},
                     {"required", Json::array()}};
    RequireParameters(&body, params);
    if (!inputs.at("required").empty()) body["required"].push_back("inputs");
    branches.push_back({{"if", SelectedType(definition.node_type)},
                        {"then", std::move(body)}});
  }
  if (types.empty()) return false;
  node["properties"]["type"] = {{"type", "string"}, {"enum", types}};
  node["properties"]["name"]["pattern"] = "^[^.]+$";
  node["properties"]["name"]["not"] = {{"enum", {"input", "output"}}};
  node["properties"]["inputs"]["additionalProperties"] = SourceReference();
  node["allOf"] = std::move(branches);
  return node;
}

Json Models() {
  auto models = Property(PipelineConfigStructure(), "models");
  auto& model = models["items"];
  const auto backends = PipelineCatalog::Backends();
  std::set<std::string> categories;
  for (const auto& definition : PipelineCatalog::Models())
    categories.insert(definition.model_type);
  auto backend_names = Json::array();
  for (const auto& backend : backends)
    backend_names.push_back(backend.backend_type);
  model["properties"]["type"] =
      categories.empty() ? Json(false)
                         : Json{{"type", "string"}, {"enum", categories}};
  model["properties"]["backend"]["properties"]["type"] =
      backend_names.empty() ? Json(false)
                            : Json{{"type", "string"}, {"enum", backend_names}};
  auto branches = Json::array();
  for (const auto& category : categories) {
    auto compatible = Json::array();
    for (const auto& backend : backends) {
      const auto implementations = ModelRegistry::Instance().FindImplementation(
          category, backend.backend_type);
      if (implementations.size() != 1) continue;
      compatible.push_back(backend.backend_type);
      const auto params = Fields(implementations.front().params.Fields());
      const auto backend_params = Fields(backend.params.Fields());
      auto backend_body = Json{{"properties", {{"params", backend_params}}},
                               {"required", Json::array()}};
      RequireParameters(&backend_body, backend_params);
      auto body =
          Json{{"properties", {{"params", params}, {"backend", backend_body}}},
               {"required", Json::array()}};
      RequireParameters(&body, params);
      auto pair = SelectedType(category);
      pair["properties"]["backend"] = SelectedType(backend.backend_type);
      pair["required"].push_back("backend");
      branches.push_back({{"if", std::move(pair)}, {"then", std::move(body)}});
    }
    auto allowed = compatible.empty() ? Json(false)
                                      : Json{{"enum", std::move(compatible)}};
    Json restriction = {
        {"properties", {{"backend", {{"properties", {{"type", allowed}}}}}}}};
    branches.push_back(
        {{"if", SelectedType(category)}, {"then", std::move(restriction)}});
  }
  if (!branches.empty()) model["allOf"] = std::move(branches);
  return models;
}

template <typename Definition>
Json Converter(const Definition& definition,
               const std::vector<ConfigFieldDefinition>& fields, bool output) {
  auto result = IoEntryStructure(output);
  result["properties"]["type"] = {{"const", definition.type}};
  result["properties"]["name"] = {{"const", definition.name}};
  const auto params = Fields(fields);
  result["properties"]["params"] = params;
  RequireParameters(&result, params);
  if (output) {
    const auto inputs = Inputs(definition.logical_ports);
    result["properties"]["inputs"] = inputs;
    if (!inputs.at("required").empty()) result["required"].push_back("inputs");
  }
  return result;
}

Json Io() {
  auto result = IoStructure();
  auto inputs = Json::array();
  auto outputs = Json::array();
  const auto& registry = IoConverterRegistry::Instance();
  for (const auto& definition : registry.AllInputConverters())
    inputs.push_back(Converter(definition, definition.params.Fields(), false));
  for (const auto& definition : registry.AllOutputConverters())
    outputs.push_back(Converter(
        definition, OutputConverterParameterFields(definition), true));
  result["properties"]["input"]["items"] =
      inputs.empty() ? Json(false) : Json{{"oneOf", std::move(inputs)}};
  result["properties"]["output"]["items"] =
      outputs.empty() ? Json(false) : Json{{"oneOf", std::move(outputs)}};
  return result;
}
}  // namespace

nlohmann::json BuildPipelineJsonSchema() {
  auto schema = PipelineDocumentStructure();
  auto& properties = schema["properties"];
  properties["models"] = Models();
  properties["io"] = Io();
  properties["pipeline"]["items"] = Nodes();
  schema["$schema"] = "http://json-schema.org/draft-07/schema#";
  schema["title"] = "LLM-EdgeFlow Pipeline (selected build)";
  schema["description"] =
      "Generated from registered Definitions in this build. Use validate/plan "
      "for semantic checks. Defaults are hints only.";
  return schema;
}

}  // namespace llm_edgeflow

#include "pipeline_json_schema.h"

#include <string>

#include "adapter/deployment_structure.h"
#include "contracts/json_structure.h"
#include "core/pipeline_config_structure.h"

namespace llm_edgeflow {
namespace {
using Json = nlohmann::json;

using json_structure::Object;
using json_structure::Property;

Json Choices(const Json& definitions, const char* field) {
  Json values = Json::array();
  for (const auto& definition : definitions)
    values.push_back(definition.at(field));
  if (values.empty()) return false;
  return {{"type", "string"}, {"enum", std::move(values)}};
}

Json Fields(const Json& fields) {
  Json properties = Json::object();
  Json required = Json::array();
  for (const auto& field : fields) {
    Json property = {{"type", field.at("type")}};
    for (const char* key : {"default", "minimum", "maximum", "enum"}) {
      if (field.contains(key)) property[key] = field.at(key);
    }
    if (field.contains("semantic"))
      property["description"] = field.at("semantic");
    properties[field.at("name").get<std::string>()] = std::move(property);
    if (field.at("required").get<bool>()) required.push_back(field.at("name"));
  }
  return Object(std::move(properties), std::move(required));
}

Json PortMappings(const Json& ports, const Json& mapping_shape, bool inputs) {
  Json properties = Json::object();
  Json required = Json::array();
  for (const auto& port : ports) {
    auto property = mapping_shape.at("additionalProperties");
    property["description"] =
        port.at("type_id").get<std::string>() +
        (inputs ? "; explicit Blackboard key."
                : "; Blackboard key (defaults to port name).");
    properties[port.at("key").get<std::string>()] = std::move(property);
    if (inputs && port.at("required").get<bool>())
      required.push_back(port.at("key"));
  }
  return Object(std::move(properties), std::move(required));
}

Json Node(const Json& definition) {
  auto config = Fields(definition.at("config_fields"));
  auto result = Property(PipelineConfigStructure(), "pipeline").at("items");
  if (!config.at("required").empty()) result["required"].push_back("config");
  auto& properties = result["properties"];
  properties["node_type"] = {{"const", definition.at("node_type")}};
  properties["config"] = std::move(config);
  for (const char* direction : {"inputs", "outputs"}) {
    properties[direction] =
        PortMappings(definition.at(direction), properties.at(direction),
                     std::string(direction) == "inputs");
    if (!properties[direction].at("required").empty())
      result["required"].push_back(direction);
  }
  result["title"] = definition.at("node_type");
  result["description"] = definition.at("description");
  return result;
}

Json ConfigBranch(const Json& definition, const char* selector,
                  const char* definition_key, const char* config_key) {
  auto config = Fields(definition.at("config_fields"));
  Json body = {{"properties", {{config_key, config}}}};
  if (!config.at("required").empty()) body["required"] = {config_key};
  return {{"if",
           {{"properties",
             {{selector, {{"const", definition.at(definition_key)}}}}},
            {"required", {selector}}}},
          {"then", std::move(body)}};
}

Json Models(const Json& catalog) {
  auto result = Property(PipelineConfigStructure(), "models");
  auto& model = result["items"];
  model["properties"]["model_type"] =
      Choices(catalog.at("models"), "model_type");
  model["properties"]["backend"] =
      Choices(catalog.at("backends"), "backend_type");
  Json branches = Json::array();
  for (const auto& definition : catalog.at("models")) {
    branches.push_back(
        ConfigBranch(definition, "model_type", "model_type", "model_config"));
  }
  for (const auto& definition : catalog.at("backends")) {
    branches.push_back(
        ConfigBranch(definition, "backend", "backend_type", "backend_config"));
  }
  if (!branches.empty()) model["allOf"] = std::move(branches);
  return result;
}

Json Deployment(const Json& catalog) {
  auto result = DeploymentStructure();
  result["properties"]["io"]["properties"]["io_binding"] =
      Choices(catalog.at("io_bindings"), "binding_id");
  return result;
}
}  // namespace

nlohmann::json BuildPipelineJsonSchema(const nlohmann::json& catalog) {
  Json nodes = Json::array();
  for (const auto& definition : catalog.at("nodes"))
    nodes.push_back(Node(definition));
  Json node_schema =
      nodes.empty() ? Json(false) : Json{{"oneOf", std::move(nodes)}};
  auto schema = PipelineDocumentStructure();
  auto& properties = schema["properties"];
  properties["models"] = Models(catalog);
  properties["deployment"] = Deployment(catalog);
  properties["pipeline"]["items"] = std::move(node_schema);
  schema["$schema"] = "http://json-schema.org/draft-07/schema#";
  schema["title"] = "LLM-EdgeFlow Pipeline (selected build)";
  schema["description"] =
      "Generated from this tool's Catalog. Use validate/plan for semantic "
      "checks; "
      "refresh after changing registrations or build options. Defaults are "
      "hints only.";
  return schema;
}

}  // namespace llm_edgeflow

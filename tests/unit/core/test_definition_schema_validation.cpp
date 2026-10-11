#include <gtest/gtest.h>

#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "core/node_definition_validation.h"

namespace llm_edgeflow {
namespace {

NodeDefinition ValidDefinition() {
  NodeDefinition definition;
  definition.node_type = "definition_probe";
  definition.inputs = {NodePortDefinition{"text", "TextBatch"}};
  definition.outputs = {NodePortDefinition{"answer", "TextBatch", true, "1:1",
                                           "preserve", "request", "text"}};
  definition.port_constraints = {
      {PortConstraintKind::kAtMostOneOf, {"text", "answer"}}};
  definition.control_commands = {{77, "update", "", {{"type", "object"}}}};
  ConfigFieldDefinition limit;
  limit.name = "limit";
  limit.kind = ConfigValueKind::kInteger;
  limit.default_value = 3;
  limit.minimum = 0;
  limit.maximum = 10;
  ConfigFieldDefinition reference;
  reference.name = "bind_generator";
  reference.required = true;
  ConfigFieldDefinition mode;
  mode.name = "mode";
  mode.enum_values = {"a", "b"};
  mode.default_value = "a";
  definition.config_fields = {limit, reference, mode};
  definition.model_dependencies = {{"generator", "llm", "bind_generator"}};
  return definition;
}

TEST(NodeDefinitionTest,
     AcceptsDeclaredFlowValuesAndExistingFollowLifetimeAnchor) {
  for (const std::string cardinality : {"1:1", "1:N", "N:1", "N:M"}) {
    SCOPED_TRACE(cardinality);
    auto definition = ValidDefinition();
    definition.inputs[0].cardinality = cardinality;
    definition.outputs[0].cardinality = cardinality;
    std::string error = "stale";
    EXPECT_TRUE(ValidateNodeDefinitionStructure(definition, &error));
    EXPECT_TRUE(error.empty());
  }
  for (const std::string provenance :
       {"preserve", "generate_sub_id", "aggregate", "independent"}) {
    SCOPED_TRACE(provenance);
    auto definition = ValidDefinition();
    definition.inputs[0].provenance_policy = provenance;
    definition.outputs[0].provenance_policy = provenance;
    EXPECT_TRUE(ValidateNodeDefinitionStructure(definition));
  }
  for (const std::string lifetime : {"request", "session"}) {
    SCOPED_TRACE(lifetime);
    auto definition = ValidDefinition();
    definition.inputs[0].lifetime = lifetime;
    definition.outputs[0].lifetime = lifetime;
    EXPECT_TRUE(ValidateNodeDefinitionStructure(definition));
  }
  auto grouped = ValidDefinition();
  grouped.port_constraints = {PortGroupConstraint::Groups(
      PortConstraintKind::kExactOneGroupOf, {{"text"}, {"answer"}})};
  EXPECT_TRUE(ValidateNodeDefinitionStructure(grouped));
}

TEST(NodeDefinitionTest, RejectsMalformedDeclarationsFromOneValidBase) {
  struct Case {
    const char* name;
    std::function<void(NodeDefinition&)> change;
    const char* error;
  };
  const std::vector<Case> cases{
      {"empty node type", [](auto& d) { d.node_type.clear(); },
       "Invalid or duplicate NodeDefinition"},
      {"empty input name", [](auto& d) { d.inputs[0].logical_name.clear(); },
       "Port key cannot be empty"},
      {"empty output name", [](auto& d) { d.outputs[0].logical_name.clear(); },
       "Port key cannot be empty"},
      {"empty input type", [](auto& d) { d.inputs[0].type_id.clear(); },
       "type_id cannot be empty"},
      {"empty output type", [](auto& d) { d.outputs[0].type_id.clear(); },
       "type_id cannot be empty"},
      {"duplicate input", [](auto& d) { d.inputs.push_back(d.inputs[0]); },
       "Duplicate port key"},
      {"duplicate output", [](auto& d) { d.outputs.push_back(d.outputs[0]); },
       "Duplicate port key"},
      {"invalid cardinality", [](auto& d) { d.inputs[0].cardinality = "2:3"; },
       "Invalid port cardinality"},
      {"invalid provenance",
       [](auto& d) { d.outputs[0].provenance_policy = "clone"; },
       "Invalid port provenance"},
      {"invalid lifetime", [](auto& d) { d.inputs[0].lifetime = "persistent"; },
       "Invalid port lifetime"},
      {"input cannot follow lifetime",
       [](auto& d) { d.inputs[0].lifetime_from_input = "text"; },
       "Invalid or duplicate NodeDefinition"},
      {"output anchor must exist",
       [](auto& d) { d.outputs[0].lifetime_from_input = "absent"; },
       "Invalid or duplicate NodeDefinition"},
      {"logical input cannot contain dot",
       [](auto& d) {
         d.inputs[0].logical_name = "input.text";
         d.outputs[0].lifetime_from_input = "input.text";
         d.port_constraints[0].ports = {"input.text", "answer"};
       },
       "Invalid or duplicate NodeDefinition"},
      {"logical output cannot contain dot",
       [](auto& d) {
         d.outputs[0].logical_name = "node.answer";
         d.port_constraints[0].ports = {"text", "node.answer"};
       },
       "Invalid or duplicate NodeDefinition"},
      {"constraint needs ports",
       [](auto& d) { d.port_constraints[0].ports.clear(); },
       "Invalid or duplicate NodeDefinition"},
      {"constraint references declared ports",
       [](auto& d) { d.port_constraints[0].ports = {"absent"}; },
       "Invalid or duplicate NodeDefinition"},
      {"group constraint needs groups",
       [](auto& d) {
         d.port_constraints = {PortGroupConstraint::Groups(
             PortConstraintKind::kExactOneGroupOf, {})};
       },
       "Invalid or duplicate NodeDefinition"},
      {"group cannot be empty",
       [](auto& d) {
         d.port_constraints = {PortGroupConstraint::Groups(
             PortConstraintKind::kExactOneGroupOf, {{}})};
       },
       "Invalid or duplicate NodeDefinition"},
      {"group reference must exist",
       [](auto& d) {
         d.port_constraints = {PortGroupConstraint::Groups(
             PortConstraintKind::kExactOneGroupOf, {{"absent"}})};
       },
       "Invalid or duplicate NodeDefinition"},
      {"Control ID must be positive",
       [](auto& d) { d.control_commands[0].cmd_id = 0; },
       "Invalid or duplicate NodeDefinition"},
      {"Control name nonempty",
       [](auto& d) { d.control_commands[0].name.clear(); },
       "Invalid or duplicate NodeDefinition"},
      {"duplicate Control ID",
       [](auto& d) { d.control_commands.emplace_back(77, "another"); },
       "Invalid or duplicate NodeDefinition"},
      {"duplicate Control name",
       [](auto& d) { d.control_commands.emplace_back(78, "update"); },
       "Invalid or duplicate NodeDefinition"},
      {"Control schema is object",
       [](auto& d) { d.control_commands[0].payload_schema = 7; },
       "schema must be an object"},
      {"Control schema rejects unsupported keyword",
       [](auto& d) { d.control_commands[0].payload_schema["pattern"] = "x"; },
       "Unsupported Control schema keyword"},
      {"reversed config bounds",
       [](auto& d) { d.config_fields[0].minimum = 11; },
       "Minimum bound cannot be greater"},
      {"nonfinite declaration",
       [](auto& d) {
         d.config_fields[0].maximum = std::numeric_limits<double>::infinity();
       },
       "bounds must be finite"},
      {"duplicate config field",
       [](auto& d) { d.config_fields.push_back(d.config_fields[0]); },
       "Duplicate config field name"},
      {"enum default outside declaration",
       [](auto& d) { d.config_fields[2].default_value = "c"; },
       "Invalid default for mode"},
      {"dependency slot nonempty",
       [](auto& d) { d.model_dependencies[0].name.clear(); },
       "must be non-empty"},
      {"dependency type nonempty",
       [](auto& d) { d.model_dependencies[0].model_type.clear(); },
       "must be non-empty"},
      {"dependency reference nonempty",
       [](auto& d) { d.model_dependencies[0].config_field.clear(); },
       "must be non-empty"},
      {"duplicate dependency slot",
       [](auto& d) {
         d.model_dependencies.push_back({"generator", "llm", "mode"});
       },
       "Duplicate model dependency name"},
      {"duplicate dependency config field",
       [](auto& d) {
         d.model_dependencies.push_back({"second", "llm", "bind_generator"});
       },
       "Duplicate model dependency config_field"},
      {"dependency reference must be declared",
       [](auto& d) { d.model_dependencies[0].config_field = "absent"; },
       "not found in config_fields"},
      {"dependency reference is string",
       [](auto& d) { d.config_fields[1].kind = ConfigValueKind::kInteger; },
       "must be of string kind"},
      {"dependency reference required",
       [](auto& d) { d.config_fields[1].required = false; },
       "must be required and have no default"},
      {"dependency reference cannot default",
       [](auto& d) { d.config_fields[1].default_value = "chosen"; },
       "must be required and have no default"},
  };
  ASSERT_TRUE(ValidateNodeDefinitionStructure(ValidDefinition()));
  for (const auto& row : cases) {
    SCOPED_TRACE(row.name);
    auto definition = ValidDefinition();
    row.change(definition);
    std::string error;
    EXPECT_FALSE(ValidateNodeDefinitionStructure(definition, &error));
    EXPECT_NE(error.find(row.error), std::string::npos) << error;
  }
}

}  // namespace
}  // namespace llm_edgeflow

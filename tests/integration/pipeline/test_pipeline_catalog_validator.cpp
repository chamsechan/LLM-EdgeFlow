#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"
#include "nodes/authoring.h"
#include "tests/support/registry_test_access.h"

namespace llm_edgeflow {
namespace {

struct CatalogInputs {
  const TextBatch* text = nullptr;
  const TextBatch* extra = nullptr;
};
struct CatalogParams {
  int limit = 0;
  std::optional<std::string> note;
};
struct CatalogModels {
  LlmCall generator;
};

Parameters<CatalogParams> CatalogParameters() {
  return Parameters<CatalogParams>(
      {Field("limit", &CatalogParams::limit).Default(7).Range(2, 20),
       Field("note", &CatalogParams::note)});
}

TEST(PipelineCatalogTest, NativeSnapshotExportsTheRegisteredTypedSpec) {
  test_support::RegistryTestAccess::ScopedNodeState saved;
  test_support::RegistryTestAccess::ResetNodes();
  auto spec =
      MakeNodeSpec(
          InputsOf<CatalogInputs>({Required("text", &CatalogInputs::text)}),
          PreservedOutput<TextBatch>(
              "answer", "text",
              PortFlow{"1:1", "preserve", FollowLifetime("text")}),
          CatalogParameters(),
          ModelsOf<CatalogModels>({Model("generator", "bind_generator",
                                         &CatalogModels::generator)}),
          [](const CatalogInputs& input, const CatalogParams&,
             const CatalogModels&) {
            return NodeResult<TextBatch>::Success(*input.text);
          })
          .Description("literal catalog description")
          .ParallelSafe(true)
          .WithControls({ReplaceFields(201, "replace_limit", {"limit"})});
  const auto definition = spec.BuildDefinition("catalog_probe");
  ASSERT_TRUE(NodeRegistry::Instance().Register(
      "catalog_probe",
      [spec] {
        return std::make_unique<AuthorNode<decltype(spec)>>("catalog_probe",
                                                            spec);
      },
      definition));
  auto snapshot = PipelineCatalog::Snapshot();
  ASSERT_NE(snapshot.FindNode("catalog_probe"), nullptr);
  // Catalog JSON must retain this native snapshot after the live registry
  // changes.
  test_support::RegistryTestAccess::ResetNodes();
  const auto catalog = PipelineCatalog::ToJson(snapshot);
  const auto expected = nlohmann::json::parse(R"json([
    {
      "node_type": "catalog_probe", "category": "custom",
      "description": "literal catalog description", "parallel_safe": true,
      "inputs": [{"key": "text", "type_id": "TextBatch", "required": true,
        "cardinality": "1:1", "provenance_policy": "preserve", "lifetime": "request"}],
      "outputs": [{"key": "answer", "type_id": "TextBatch", "required": true,
        "cardinality": "1:1", "provenance_policy": "preserve", "lifetime": "request",
        "lifetime_from_input": "text"}],
      "port_constraints": [],
      "config_fields": [
        {"name": "limit", "type": "integer", "required": false,
         "default": 7, "minimum": 2, "maximum": 20},
        {"name": "note", "type": "string", "required": false},
        {"name": "bind_generator", "type": "string", "required": true,
         "semantic": "引用 models[].name；所选模型的类别必须是 llm"}
      ],
      "model_dependencies": [{"name": "generator", "model_type": "llm",
                              "config_field": "bind_generator"}],
      "control_commands": [{"cmd_id": 201, "name": "replace_limit",
        "description": "replace_limit",
        "payload_schema": {"type": "object", "minProperties": 1,
          "additionalProperties": false,
          "properties": {"limit": {"type": "integer", "default": 7,
                                   "minimum": 2, "maximum": 20}}}}]
    }
  ])json");
  EXPECT_EQ(catalog.at("nodes"), expected);
}

TEST(PipelineCatalogTest,
     RegisteredSemanticCallbacksReceiveNormalizedPlanAndBindings) {
  test_support::RegistryTestAccess::ScopedNodeState saved;
  test_support::RegistryTestAccess::ResetNodes();
  int creations = 0;
  std::vector<int> prepared_limits;
  std::vector<std::string> lifetimes;
  std::vector<std::unordered_set<std::string>> connections;
  auto params =
      CatalogParameters()
          .Prepare(
              [&](CatalogParams* p, const BindingFacts& facts, std::string*) {
                prepared_limits.push_back(p->limit);
                lifetimes.push_back(facts.InputLifetime("text"));
                return true;
              })
          .Validate([](const CatalogParams& p, std::string* error) {
            if (p.note != "reject") return true;
            *error = "literal catalog semantic rejection";
            return false;
          })
          .ValidateBindings([&](const CatalogParams& p,
                                const std::unordered_set<std::string>& inputs,
                                std::string* error) {
            connections.push_back(inputs);
            if (p.note != "needs_extra" || inputs.count("extra")) return true;
            *error = "literal catalog binding rejection";
            return false;
          });
  auto spec = MakeNodeSpec(
      InputsOf<CatalogInputs>({Required("text", &CatalogInputs::text),
                               Optional("extra", &CatalogInputs::extra)}),
      PreservedOutput<TextBatch>(
          "answer", "text",
          PortFlow{"1:1", "preserve", FollowLifetime("text")}),
      params, [](const CatalogInputs& input, const CatalogParams&) {
        return NodeResult<TextBatch>::Success(*input.text);
      });
  ASSERT_TRUE(NodeRegistry::Instance().Register(
      "semantic_probe",
      [&, spec] {
        ++creations;
        return std::make_unique<AuthorNode<decltype(spec)>>("semantic_probe",
                                                            spec);
      },
      spec.BuildDefinition("semantic_probe")));
  PipelineIoBoundary boundary;
  boundary.input_published_ports = {IoPortDefinition{
      "input.source", "TextBatch", true, "1:1", "preserve", "session"}};
  boundary.output_consumed_ports = {IoPortDefinition{
      "native.answer", "TextBatch", true, "1:1", "preserve", "session"}};
  const std::vector<std::pair<nlohmann::json, std::string>> cases{
      {nlohmann::json::object(), ""},
      {{{"note", "reject"}}, "literal catalog semantic rejection"},
      {{{"note", "needs_extra"}}, "literal catalog binding rejection"},
  };
  for (const auto& row : cases) {
    SCOPED_TRACE(row.first.dump());
    auto plan = PipelineValidator::ValidateAndPlan(
        {{"pipeline",
          nlohmann::json::array({{{"type", "semantic_probe"},
                                  {"name", "native"},
                                  {"params", row.first},
                                  {"inputs", {{"text", "input.source"}}}}})}},
        boundary);
    EXPECT_EQ(plan.report.ok, row.second.empty());
    EXPECT_EQ(creations, 0);
    if (row.second.empty()) {
      ASSERT_TRUE(plan.report.ok);
      const auto& node = plan.node_plans.at("native");
      EXPECT_EQ(node.normalized_params, nlohmann::json({{"limit", 7}}));
      ASSERT_NE(node.FindPort("answer", PortDirection::kOutput), nullptr);
      EXPECT_EQ(node.FindPort("answer", PortDirection::kOutput)->lifetime,
                "session");
    } else {
      ASSERT_EQ(plan.report.diagnostics.size(), 1U);
      const auto& diagnostic = plan.report.diagnostics[0];
      EXPECT_EQ(diagnostic.code, DiagnosticCode::kInvalidCombination);
      EXPECT_EQ(diagnostic.path, "/pipeline/0/params");
      EXPECT_EQ(diagnostic.node_name, "native");
      EXPECT_EQ(diagnostic.message, row.second);
      const auto serialized = diagnostic.ToJson();
      EXPECT_EQ(serialized.at("code"), "INVALID_COMBINATION");
      EXPECT_EQ(serialized.at("path"), diagnostic.path);
      EXPECT_EQ(serialized.at("message"), row.second);
      EXPECT_EQ(serialized.at("node_name"), "native");
    }
  }
  EXPECT_EQ(prepared_limits, (std::vector<int>{7, 7, 7}));
  EXPECT_EQ(lifetimes,
            (std::vector<std::string>{"session", "session", "session"}));
  ASSERT_EQ(connections.size(), 2U);
  for (const auto& inputs : connections)
    EXPECT_EQ(inputs, std::unordered_set<std::string>{"text"});
}

}  // namespace
}  // namespace llm_edgeflow

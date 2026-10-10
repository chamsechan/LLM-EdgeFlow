#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "adapter/deployment_preparation.h"
#include "adapter/io_converter_registry.h"
#include "adapter/io_plan_resolver.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "adapter/pipeline_document.h"
#include "adapter/shared_algorithm_runtime.h"
#include "cli/pipeline_remediation.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"
#include "nodes/node_base.h"
#include "tests/support/node_test_utils.h"
#include "tests/support/pipeline_test_utils.h"
#include "tests/support/registry_test_access.h"

namespace llm_edgeflow {
namespace {

class StudioCatalogProbeNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "studio_catalog_probe";
  bool Init(const NodeInitContext&) override { return true; }
  int Process(AlgContext*) override { return 0; }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};

NodeDefinition StudioCatalogProbeDefinition() {
  NodeDefinition definition;
  definition.node_type = StudioCatalogProbeNode::kNodeType;
  definition.category = "test";
  definition.description = "Catalog auto-discovery probe";
  return definition;
}

REGISTER_NODE_WITH_DEFINITION(StudioCatalogProbeNode,
                              StudioCatalogProbeDefinition());

PreparedDeployment PrepareExternalFixtureForCore(
    const nlohmann::json& document) {
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  EXPECT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
      << diagnostic.code << " " << diagnostic.path << " " << diagnostic.message;
  return prepared;
}

PreparedDeployment LoadRegistrationFixture(const std::string& path) {
  std::ifstream stream(path);
  EXPECT_TRUE(stream.is_open()) << path;
  nlohmann::json document;
  stream >> document;
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  EXPECT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
      << diagnostic.message;
  return prepared;
}

PreparedDeployment UnknownProducerFixture(size_t known_producers = 0) {
  auto fixture =
      LoadRegistrationFixture("configs/pipeline_keyword_match_rules.json");
  auto& pipeline = fixture.neutral_pipeline_json["pipeline"];
  const auto prototype = pipeline[0];
  auto unknown = prototype;
  unknown["name"] = "unknown";
  unknown["type"] = "text_rule_matc";
  pipeline.push_back(unknown);
  for (size_t i = 0; i < known_producers; ++i) {
    auto known = prototype;
    known["name"] = "known_" + std::to_string(i);
    pipeline.push_back(known);
  }
  auto consumer = prototype;
  consumer["name"] = "consumer";
  consumer["inputs"]["text"] =
      known_producers == 0 ? "unknown.matches" : "known_0.matches";
  pipeline.push_back(consumer);
  return fixture;
}

const ValidationDiagnostic* FindDiagnostic(const ValidationReport& report,
                                           DiagnosticCode code,
                                           const std::string& path = {}) {
  auto it = std::find_if(
      report.diagnostics.begin(), report.diagnostics.end(), [&](const auto& d) {
        return d.code == code && (path.empty() || d.path == path);
      });
  return it == report.diagnostics.end() ? nullptr : &*it;
}

TEST(PipelineValidatorTest,
     UnknownModelTypeHasRemediationWithoutReferenceCascade) {
  auto fixture = LoadRegistrationFixture(
      "demo/fixtures/mock/pipeline_entity_extract_custom.json");
  fixture.neutral_pipeline_json["models"][0]["type"] = "llmm";
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  ASSERT_EQ(report.diagnostics.size(), 1u) << report.ToJson().dump(2);
  const auto* d = FindDiagnostic(report, DiagnosticCode::kUnknownModelType);
  ASSERT_NE(d, nullptr);
  ASSERT_TRUE(d->remediation.has_value());
  EXPECT_EQ(d->remediation->cause, RemediationCause::kUnknownModelType);
  EXPECT_EQ(d->message, "Unknown model type: llmm");
  ASSERT_FALSE(d->suggestions.empty());
  EXPECT_EQ(d->suggestions.front(), "llm");
  EXPECT_EQ(d->remediation->facts["candidate_model_types"], d->suggestions);
  const auto names = d->remediation->facts["registered_model_types"]
                         .get<std::vector<std::string>>();
  EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
  EXPECT_EQ(d->remediation->facts["model_name"], "entity_llm");
  EXPECT_EQ(FindDiagnostic(report, DiagnosticCode::kUnknownModelReference),
            nullptr);
  EXPECT_EQ(d->remediation->summary.find("alg_pipeline_tool"),
            std::string::npos);
}

TEST(PipelineValidatorTest, UndeclaredModelReferenceIsStillReported) {
  auto fixture = LoadRegistrationFixture(
      "demo/fixtures/mock/pipeline_entity_extract_custom.json");
  fixture.neutral_pipeline_json["pipeline"][0]["params"]["bind_model"] =
      "undeclared";
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kUnknownModelReference,
                           "/pipeline/0/params/bind_model"),
            nullptr);
}

TEST(PipelineValidatorTest, UnknownBackendHasRemediation) {
  auto fixture = LoadRegistrationFixture(
      "demo/fixtures/mock/pipeline_entity_extract_custom.json");
  fixture.neutral_pipeline_json["models"][0]["backend"]["type"] =
      "test_causal_lm_backnd";
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  ASSERT_EQ(report.diagnostics.size(), 1u) << report.ToJson().dump(2);
  const auto& d = report.diagnostics.front();
  EXPECT_EQ(d.code, DiagnosticCode::kUnknownBackend);
  EXPECT_EQ(d.path, "/models/0/backend/type");
  ASSERT_TRUE(d.remediation.has_value());
  EXPECT_EQ(d.remediation->cause, RemediationCause::kUnknownBackend);
  ASSERT_FALSE(d.suggestions.empty());
  EXPECT_EQ(d.suggestions.front(), "test_causal_lm_backend");
  EXPECT_EQ(d.remediation->facts["candidate_backends"], d.suggestions);
  const auto names = d.remediation->facts["registered_backends"]
                         .get<std::vector<std::string>>();
  ASSERT_FALSE(names.empty());
  EXPECT_EQ(names, d.suggestions);
  EXPECT_EQ(std::set<std::string>(names.begin(), names.end()).size(),
            names.size());
  for (const auto& backend : names) {
    EXPECT_FALSE(
        ModelRegistry::Instance().FindImplementation("llm", backend).empty())
        << backend;
  }
}

TEST(PipelineValidatorTest, UnknownNodeTypeSuppressesMissingOutputCascade) {
  auto fixture =
      LoadRegistrationFixture("configs/pipeline_keyword_match_rules.json");
  fixture.neutral_pipeline_json["pipeline"][0]["type"] = "text_rule_matc";
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  ASSERT_EQ(report.diagnostics.size(), 1u) << report.ToJson().dump(2);
  const auto* d = FindDiagnostic(report, DiagnosticCode::kUnknownNodeType);
  ASSERT_NE(d, nullptr);
  ASSERT_TRUE(d->remediation.has_value());
  EXPECT_EQ(d->remediation->cause, RemediationCause::kUnknownNodeType);
  EXPECT_EQ(d->suggestions, std::vector<std::string>{"text_rule_match"});
  EXPECT_EQ(d->remediation->facts["candidate_node_types"], d->suggestions);
  EXPECT_NE(d->remediation->summary.find("重新构建"), std::string::npos);
}

TEST(PipelineValidatorTest, DistantUnknownNamesHaveNoSuggestions) {
  auto fixture =
      LoadRegistrationFixture("configs/pipeline_keyword_match_rules.json");
  fixture.neutral_pipeline_json["pipeline"][0]["type"] = "ZZZZZZZZZZZZZZZZZZZZ";
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  const auto* d = FindDiagnostic(report, DiagnosticCode::kUnknownNodeType);
  ASSERT_NE(d, nullptr);
  ASSERT_TRUE(d->remediation.has_value());
  EXPECT_TRUE(d->suggestions.empty());
  EXPECT_EQ(d->remediation->facts["candidate_node_types"],
            nlohmann::json::array());
}

TEST(PipelineValidatorTest, MissingOutputProducerIsReportedOnce) {
  auto fixture =
      LoadRegistrationFixture("configs/pipeline_keyword_match_rules.json");
  fixture.neutral_pipeline_json["pipeline"] = {
      {{"name", "probe"}, {"type", "studio_catalog_probe"}}};
  fixture.io_boundary.output_consumed_ports[0].blackboard_key.clear();
  fixture.io_boundary.output_consumed_ports[0].has_binding = false;
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  const auto* d =
      FindDiagnostic(report, DiagnosticCode::kMissingOutputProducer);
  ASSERT_NE(d, nullptr) << report.ToJson().dump(2);
  EXPECT_EQ(d->path, "/io/output/0/inputs");
  ASSERT_TRUE(d->remediation.has_value());
  EXPECT_EQ(d->remediation->cause, RemediationCause::kMissingOutputProducer);
  EXPECT_EQ(d->remediation->facts.at("bound_key"), "");
  EXPECT_EQ(std::count_if(report.diagnostics.begin(), report.diagnostics.end(),
                          [](const auto& item) {
                            return item.code ==
                                   DiagnosticCode::kMissingOutputProducer;
                          }),
            1);
}

TEST(PipelineValidatorTest, UnknownConfigFieldSuggestionsAreRanked) {
  auto fixture =
      LoadRegistrationFixture("configs/pipeline_keyword_match_rules.json");
  auto& config = fixture.neutral_pipeline_json["pipeline"][0]["params"];
  config["categoriess"] = config["categories"];
  config.erase("categories");
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  const auto* d = FindDiagnostic(report, DiagnosticCode::kUnknownConfigField);
  ASSERT_NE(d, nullptr);
  ASSERT_TRUE(d->remediation.has_value());
  ASSERT_FALSE(d->suggestions.empty());
  EXPECT_EQ(d->suggestions.front(), "categories");
  EXPECT_EQ(d->remediation->facts["candidate_fields"], d->suggestions);
}

TEST(PipelineValidatorTest, ExplainRepairsUnknownFieldInsideRuleArray) {
  auto fixture =
      LoadRegistrationFixture("configs/pipeline_keyword_match_rules.json");
  auto& root = fixture.neutral_pipeline_json;
  auto& rules = root["pipeline"][0]["params"]["rules"];
  rules = {{{"id", "vip"},
            {"pattern", "VIP"},
            {"category", "PRIORITY"},
            {"score", 0.75}},
           {{"id", "ordinary"},
            {"pattern", "check"},
            {"category", "ORDINARY"},
            {"score", 0.5}}};
  const auto expected = root;
  ASSERT_TRUE(PipelineValidator::Validate(root, fixture.io_boundary).ok);
  rules[0]["patern"] = rules[0]["pattern"];
  rules[0].erase("pattern");
  const auto basic = PipelineValidator::Validate(root, fixture.io_boundary);
  const std::string path = "/pipeline/0/params/rules/0/patern";
  const auto* basic_diagnostic =
      FindDiagnostic(basic, DiagnosticCode::kUnknownConfigField, path);
  ASSERT_NE(basic_diagnostic, nullptr) << basic.ToJson().dump(2);
  ASSERT_FALSE(basic_diagnostic->suggestions.empty());
  EXPECT_EQ(basic_diagnostic->suggestions.front(), "pattern");
  EXPECT_EQ(std::find(basic_diagnostic->suggestions.begin(),
                      basic_diagnostic->suggestions.end(), "rules"),
            basic_diagnostic->suggestions.end());

  const auto report = ExplainPipeline(root, fixture.io_boundary);
  const auto* diagnostic =
      FindDiagnostic(report, DiagnosticCode::kUnknownConfigField, path);
  ASSERT_NE(diagnostic, nullptr) << report.ToJson().dump(2);
  ASSERT_TRUE(diagnostic->remediation.has_value());
  const auto& remediation = *diagnostic->remediation;
  EXPECT_EQ(remediation.facts.at("field"), "patern");
  EXPECT_EQ(remediation.facts.at("candidate_fields"),
            basic_diagnostic->suggestions);
  ASSERT_FALSE(remediation.fixes.empty());
  const auto& fix = remediation.fixes.front();
  EXPECT_EQ(fix.verification, "pipeline_valid");
  EXPECT_TRUE(std::any_of(fix.patch.begin(), fix.patch.end(),
                          [&](const auto& operation) {
                            return operation.value("op", "") == "move" &&
                                   operation.value("from", "") == path &&
                                   operation.value("path", "") ==
                                       "/pipeline/0/params/rules/0/pattern";
                          }));
  const auto patched = root.patch(fix.patch);
  EXPECT_EQ(patched, expected);
  const auto verified =
      PipelineValidator::Validate(patched, fixture.io_boundary);
  EXPECT_TRUE(verified.ok) << verified.ToJson().dump(2);
}

TEST(PipelineValidatorTest, ExplainRepairsFieldInsideMapWithEscapedKey) {
  auto fixture = LoadRegistrationFixture(
      "demo/fixtures/mock/pipeline_entity_extract_custom.json");
  auto& root = fixture.neutral_pipeline_json;
  auto& generator = root["pipeline"][0];
  generator["type"] = "llm_generate";
  generator["params"] = {{"bind_model", "entity_llm"},
                         {"endpoints",
                          {{"answer/~draft", {{"prompt", "Answer: {{input}}"}}},
                           {"untouched", {{"prompt", "Keep: {{input}}"}}}}}};
  root["pipeline"][1]["inputs"]["text"] = "generate_entities.text";
  const auto expected = root;
  ASSERT_TRUE(PipelineValidator::Validate(root, fixture.io_boundary).ok);
  auto& endpoint = generator["params"]["endpoints"]["answer/~draft"];
  endpoint["promt"] = endpoint["prompt"];
  endpoint.erase("prompt");
  const std::string parent = "/pipeline/0/params/endpoints/answer~1~0draft/";
  const auto basic = PipelineValidator::Validate(root, fixture.io_boundary);
  const auto* basic_diagnostic = FindDiagnostic(
      basic, DiagnosticCode::kUnknownConfigField, parent + "promt");
  ASSERT_NE(basic_diagnostic, nullptr) << basic.ToJson().dump(2);
  EXPECT_EQ(basic_diagnostic->suggestions, std::vector<std::string>{"prompt"});

  const auto report = ExplainPipeline(root, fixture.io_boundary);
  const auto* diagnostic = FindDiagnostic(
      report, DiagnosticCode::kUnknownConfigField, parent + "promt");
  ASSERT_NE(diagnostic, nullptr) << report.ToJson().dump(2);
  ASSERT_TRUE(diagnostic->remediation.has_value());
  const auto& remediation = *diagnostic->remediation;
  EXPECT_EQ(remediation.facts.at("field"), "promt");
  EXPECT_EQ(remediation.facts.at("candidate_fields"),
            std::vector<std::string>{"prompt"});
  ASSERT_EQ(remediation.fixes.size(), 1u);
  const auto& fix = remediation.fixes.front();
  EXPECT_EQ(fix.verification, "pipeline_valid");
  EXPECT_TRUE(std::any_of(
      fix.patch.begin(), fix.patch.end(), [&](const auto& operation) {
        return operation.value("op", "") == "move" &&
               operation.value("from", "") == parent + "promt" &&
               operation.value("path", "") == parent + "prompt";
      }));
  const auto patched = root.patch(fix.patch);
  EXPECT_EQ(patched, expected);
  const auto verified =
      PipelineValidator::Validate(patched, fixture.io_boundary);
  EXPECT_TRUE(verified.ok) << verified.ToJson().dump(2);
}

TEST(PipelineValidatorTest, ExplainDoesNotOverwriteExistingNestedField) {
  auto fixture =
      LoadRegistrationFixture("configs/pipeline_keyword_match_rules.json");
  auto& root = fixture.neutral_pipeline_json;
  root["pipeline"][0]["params"]["rules"] = {
      {{"pattern", "keep"}, {"patern", "discard"}, {"category", "PRIORITY"}},
      {{"pattern", "untouched"}, {"category", "ORDINARY"}}};
  const auto original = root;
  const auto report = ExplainPipeline(root, fixture.io_boundary);
  const auto* diagnostic =
      FindDiagnostic(report, DiagnosticCode::kUnknownConfigField,
                     "/pipeline/0/params/rules/0/patern");
  ASSERT_NE(diagnostic, nullptr) << report.ToJson().dump(2);
  ASSERT_TRUE(diagnostic->remediation.has_value());
  EXPECT_EQ(diagnostic->remediation->facts.at("field"), "patern");
  ASSERT_FALSE(diagnostic->suggestions.empty());
  EXPECT_EQ(diagnostic->suggestions.front(), "pattern");
  for (const auto& fix : diagnostic->remediation->fixes) {
    SCOPED_TRACE(fix.id);
    auto patched = root.patch(fix.patch);
    const auto& rule = patched["pipeline"][0]["params"]["rules"][0];
    EXPECT_EQ(rule.at("pattern"), "keep");
    EXPECT_EQ(rule.at("category"), "PRIORITY");
    const auto verified =
        PipelineValidator::Validate(patched, fixture.io_boundary);
    EXPECT_TRUE(verified.ok) << verified.ToJson().dump(2);
    patched["pipeline"][0]["params"]["rules"][0] =
        original["pipeline"][0]["params"]["rules"][0];
    EXPECT_EQ(patched, original);
  }
  EXPECT_EQ(root, original);
}

TEST(PipelineValidatorTest, NestedRangeAndTypeRemediationUseLeafDefinition) {
  auto fixture =
      LoadRegistrationFixture("configs/pipeline_keyword_match_rules.json");
  auto& root = fixture.neutral_pipeline_json;
  root["pipeline"][0]["params"]["rules"] = {
      {{"pattern", "VIP"}, {"score", 0.75}}};
  for (const auto& invalid :
       std::vector<std::pair<nlohmann::json, DiagnosticCode>>{
           {-0.25, DiagnosticCode::kConfigFieldRange},
           {"invalid", DiagnosticCode::kConfigFieldType}}) {
    SCOPED_TRACE(invalid.first.dump());
    root["pipeline"][0]["params"]["rules"][0]["score"] = invalid.first;
    const auto report = ValidateWithRemediation(root, fixture.io_boundary);
    const auto* diagnostic = FindDiagnostic(report, invalid.second,
                                            "/pipeline/0/params/rules/0/score");
    ASSERT_NE(diagnostic, nullptr) << report.ToJson().dump(2);
    ASSERT_TRUE(diagnostic->remediation.has_value());
    const auto& remediation = *diagnostic->remediation;
    EXPECT_EQ(remediation.cause, RemediationCause::kInvalidConfigValue);
    EXPECT_EQ(remediation.facts.at("field"), "score");
    EXPECT_EQ(remediation.facts.at("expected_type"), "number");
    EXPECT_EQ(remediation.facts.at("minimum"), 0);
    EXPECT_EQ(remediation.facts.at("maximum"), 1);
  }
}

TEST(PipelineValidatorTest, UnknownNodeSoleProducerSuppressesConsumerCascade) {
  auto fixture = UnknownProducerFixture();
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  ASSERT_EQ(report.diagnostics.size(), 1u) << report.ToJson().dump(2);
  EXPECT_EQ(report.diagnostics.front().code, DiagnosticCode::kUnknownNodeType);
  EXPECT_EQ(FindDiagnostic(report, DiagnosticCode::kMissingInputProducer),
            nullptr);
}

TEST(PipelineValidatorTest, UnknownNodeDoesNotHideKnownProducerTypeMismatch) {
  auto fixture = UnknownProducerFixture(1);
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  ASSERT_EQ(report.diagnostics.size(), 2u) << report.ToJson().dump(2);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kUnknownNodeType), nullptr);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kPortTypeMismatch,
                           "/pipeline/3/inputs/text"),
            nullptr);
}

TEST(PipelineValidatorTest, UnknownNodeDoesNotHideIngressTypeMismatch) {
  auto fixture = UnknownProducerFixture();
  auto& pipeline = fixture.neutral_pipeline_json["pipeline"];
  pipeline[2]["type"] = "vector_top_k";
  pipeline[2]["params"] = nlohmann::json::object();
  pipeline[2]["inputs"] = {{"queries", "input.sentence_text"},
                           {"candidates", "input.sentence_text"},
                           {"candidate_texts", "input.sentence_text"}};
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kUnknownNodeType), nullptr);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kPortTypeMismatch,
                           "/pipeline/2/inputs/queries"),
            nullptr)
      << report.ToJson().dump(2);
}

TEST(PipelineValidatorTest, UnknownNodeDoesNotHideUnrelatedMissingInput) {
  auto fixture = UnknownProducerFixture();
  fixture.neutral_pipeline_json["pipeline"][2]["inputs"].erase("text");
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  ASSERT_EQ(report.diagnostics.size(), 2u) << report.ToJson().dump(2);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kMissingInputProducer,
                           "/pipeline/2/inputs"),
            nullptr);
}

TEST(PipelineValidatorTest, UnknownModelTypeDoesNotHideTypeMismatch) {
  auto fixture = LoadRegistrationFixture(
      "demo/fixtures/mock/pipeline_entity_extract_custom.json");
  auto& root = fixture.neutral_pipeline_json;
  root["models"][0]["type"] = "llmm";
  root["models"].push_back({{"name", "known_embedding"},
                            {"type", "embedding"},
                            {"backend", {{"type", "test_tensor_backend"}}},
                            {"file", "fixture.bin"},
                            {"params", nlohmann::json::object()}});
  root["pipeline"][0]["params"]["bind_model"] = "known_embedding";
  root["pipeline"].push_back(
      {{"name", "unresolved_llm_user"},
       {"type", "llm_generate"},
       {"params",
        {{"bind_model", "entity_llm"},
         {"endpoints", {{"answer", nlohmann::json::object()}}}}},
       {"inputs", {{"input", "input.sentence_text"}}}});
  const auto report = ValidateWithRemediation(root, fixture.io_boundary);
  ASSERT_EQ(report.diagnostics.size(), 2u) << report.ToJson().dump(2);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kUnknownModelType), nullptr);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kModelTypeMismatch,
                           "/pipeline/0/params/bind_model"),
            nullptr);
}

TEST(PipelineCatalogTest, RegisteredProductionTypesHaveDefinitions) {
  for (const auto& node_type : NodeRegistry::Instance().ListTypes()) {
    EXPECT_TRUE(PipelineCatalog::FindNode(node_type).has_value()) << node_type;
  }
  for (const auto& impl_name : ModelRegistry::Instance().ListImplNames()) {
    EXPECT_TRUE(PipelineCatalog::FindModel(impl_name).has_value()) << impl_name;
  }
  for (const auto& backend_type : BackendRegistry::Instance().ListTypes()) {
    EXPECT_TRUE(PipelineCatalog::FindBackend(backend_type).has_value())
        << backend_type;
  }
}

TEST(PipelineCatalogTest, OutputIsDeterministicAndConflictFree) {
  EXPECT_EQ(PipelineCatalog::ToJson(), PipelineCatalog::ToJson());
  std::set<std::string> node_types;
  for (const auto& definition : PipelineCatalog::Nodes()) {
    EXPECT_TRUE(node_types.insert(definition.node_type).second)
        << definition.node_type;
  }
  std::set<std::string> model_types;
  for (const auto& definition : PipelineCatalog::Models()) {
    EXPECT_TRUE(model_types.insert(definition.impl_name).second)
        << definition.impl_name;
  }
  std::set<std::string> backend_types;
  for (const auto& definition : PipelineCatalog::Backends()) {
    EXPECT_TRUE(backend_types.insert(definition.backend_type).second)
        << definition.backend_type;
  }
}

TEST(PipelineCatalogTest, DefinitionRegistrationMakesNewNodeDiscoverable) {
  const auto definition = PipelineCatalog::FindNode("studio_catalog_probe");
  ASSERT_TRUE(definition.has_value());
  EXPECT_EQ(definition->description, "Catalog auto-discovery probe");
  const auto catalog = PipelineCatalog::ToJson();
  EXPECT_TRUE(std::any_of(catalog["nodes"].begin(), catalog["nodes"].end(),
                          [](const auto& item) {
                            return item["node_type"] == "studio_catalog_probe";
                          }));
}

TEST(PipelineValidatorTest, AllRepositoryPipelinesValidate) {
  const std::filesystem::path configs("configs");
  size_t validated = 0;
  size_t skipped_optional = 0;
  size_t candidates = 0;
  for (const auto& entry : std::filesystem::directory_iterator(configs)) {
    const auto filename = entry.path().filename().string();
    if (!entry.is_regular_file() || entry.path().extension() != ".json" ||
        filename.rfind("pipeline_", 0) != 0) {
      continue;
    }
    ++candidates;
    std::ifstream stream(entry.path());
    ASSERT_TRUE(stream.is_open()) << entry.path();
    nlohmann::json pipeline;
    ASSERT_NO_THROW(stream >> pipeline) << entry.path();
    bool requires_unavailable_runtime = false;
    for (const auto& model :
         pipeline.value("models", nlohmann::json::array())) {
      if (!model.is_object()) continue;
      const std::string backend = model.at("backend").value("type", "");
      const std::string model_type = model.value("type", "");
      if ((!backend.empty() && !BackendRegistry::Instance().Has(backend)) ||
          ModelRegistry::Instance()
              .FindImplementation(model_type, backend)
              .empty()) {
        requires_unavailable_runtime = true;
        break;
      }
    }
    if (requires_unavailable_runtime) {
      ++skipped_optional;
      continue;
    }
    const auto fixture = PrepareExternalFixtureForCore(pipeline);
    const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                                fixture.io_boundary);
    EXPECT_TRUE(report.ok) << entry.path() << "\n" << report.ToJson().dump(2);
    ++validated;
  }
  EXPECT_GT(validated, 0U);
  EXPECT_EQ(validated + skipped_optional, candidates);
}

TEST(PipelineValidatorTest, CoreLeavesModelFileRulesToIntegration) {
  std::ifstream stream("demo/fixtures/mock/pipeline_doc_qa.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json pipeline;
  ASSERT_NO_THROW(stream >> pipeline);
  auto fixture = PrepareExternalFixtureForCore(pipeline);
  pipeline = fixture.neutral_pipeline_json;

  for (const std::string& model_file :
       {std::string("missing/artifact.bin"), std::string("..name/artifact.bin"),
        std::string("missing/../artifact.bin"),
        std::filesystem::absolute("missing/artifact.bin").string(),
        std::string(".."), std::string("../artifact.bin"),
        std::string("missing/../../artifact.bin"),
        std::string("..\\artifact.bin"), std::string("C:\\artifact.bin"),
        std::string("\\\\server\\share\\artifact.bin")}) {
    pipeline["models"][0]["file"] = model_file;
    const auto report = ValidateWithRemediation(pipeline, fixture.io_boundary);
    EXPECT_TRUE(report.ok) << model_file << "\n" << report.ToJson().dump(2);
  }
}

TEST(PipelineValidatorTest, ReportsConfigAndCapabilityErrors) {
  const auto boundary =
      MakeTestBoundary({{"input.sentence_text", "TextBatch"}}, {});

  const nlohmann::json pipeline = {
      {"models",
       {{{"name", "llm_model"},
         {"type", "embedding"},
         {"backend", {{"type", "test_tensor_backend"}}},
         {"file", "fixture.bin"},
         {"params", nlohmann::json::object()}}}},
      {"pipeline",
       {{{"name", "pre"},
         {"type", "text_template"},
         {"depends_on", nlohmann::json::array()}},
        {{"name", "llm"},
         {"type", "llm_generate"},
         {"depends_on", {"pre"}},
         {"params",
          {{"bind_model", "llm_model"},
           {"max_tokens", 0},
           {"invented", true},
           {"endpoints", {{"answer", nlohmann::json::object()}}}}}},
        {{"name", "post"},
         {"type", "structured_json_parse"},
         {"depends_on", {"llm"}}}}}};
  const auto report = ValidateWithRemediation(pipeline, boundary);
  EXPECT_FALSE(report.ok);
  std::set<DiagnosticCode> codes;
  for (const auto& diagnostic : report.diagnostics)
    codes.insert(diagnostic.code);
  EXPECT_TRUE(codes.count(DiagnosticCode::kUnknownConfigField));
  EXPECT_TRUE(codes.count(DiagnosticCode::kConfigFieldRange));
  EXPECT_TRUE(codes.count(DiagnosticCode::kModelTypeMismatch));

  // 验证外部 JSON 序列化一致性
  auto json_rep = report.ToJson();
  std::set<std::string> json_codes;
  for (const auto& item : json_rep["diagnostics"]) {
    json_codes.insert(item["code"].get<std::string>());
  }
  EXPECT_TRUE(json_codes.count("UNKNOWN_CONFIG_FIELD"));
  EXPECT_TRUE(json_codes.count("CONFIG_FIELD_RANGE"));
  EXPECT_TRUE(json_codes.count("MODEL_TYPE_MISMATCH"));
}

static nlohmann::json MakeSyntheticDeploymentDocForTest(
    const nlohmann::json& pipeline_json, const nlohmann::json& io_descriptor) {
  nlohmann::json synthetic = pipeline_json;
  synthetic["io"] = io_descriptor;
  return synthetic;
}

TEST(PipelineValidatorTest, SerializedModelBranchesRunInSeparateLayers) {
  const auto boundary =
      MakeTestBoundary({{"input.sentence_text", "TextBatch"}},
                       {{"post.document", "StructuredDocumentBatch"}});

  const nlohmann::json config = {
      {"max_parallel_workers", 4},
      {"models",
       {{{"name", "serialized_llm"},
         {"type", "llm"},
         {"backend", {{"type", "test_causal_lm_backend"}}},
         {"file", "fixture.gguf"}}}},
      {"pipeline",
       {{{"name", "pre"},
         {"type", "text_template"},
         {"depends_on", nlohmann::json::array()},
         {"inputs", {{"primary", "input.sentence_text"}}}},
        {{"name", "left"},
         {"type", "llm_generate"},
         {"depends_on", {"pre"}},
         {"params",
          {{"bind_model", "serialized_llm"},
           {"endpoints", {{"answer", nlohmann::json::object()}}}}},
         {"inputs", {{"input", "pre.text"}}}},
        {{"name", "right"},
         {"type", "llm_generate"},
         {"depends_on", {"pre"}},
         {"params",
          {{"bind_model", "serialized_llm"},
           {"endpoints", {{"answer", nlohmann::json::object()}}}}},
         {"inputs", {{"input", "pre.text"}}}},
        {{"name", "post"},
         {"type", "structured_json_parse"},
         {"depends_on", {"left", "right"}},
         {"inputs", {{"text", "left.text"}}}}}}};

  const auto report = ValidateWithRemediation(config, boundary);
  ASSERT_TRUE(report.ok) << report.ToJson().dump(2);
  EXPECT_EQ(report.topological_layers,
            (std::vector<std::vector<std::string>>{
                {"pre"}, {"left"}, {"right"}, {"post"}}));
  EXPECT_EQ(report.topological_order,
            (std::vector<std::string>{"pre", "left", "right", "post"}));
}

TEST(PipelineValidatorTest, InvalidConfigurationsAndDiagnosticPropagation) {
  std::ifstream stream(
      "tests/fixtures/pipelines/validation/invalid_pipeline_cases.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json fixtures;
  stream >> fixtures;

  for (const auto& test : fixtures["cases"]) {
    const auto name = test["name"].get<std::string>();
    SCOPED_TRACE(name);
    const auto& config = test["pipeline"];
    const auto& io_descriptor = test.at("io");
    std::vector<IoPortDefinition> inputs;
    std::vector<IoPortDefinition> outputs;
    const auto& converters = IoConverterRegistry::Instance();
    for (const auto& entry : io_descriptor.at("input")) {
      const auto* converter =
          converters.FindInputConverter(entry.at("type").get<std::string>(),
                                        entry.at("name").get<std::string>());
      ASSERT_NE(converter, nullptr);
      for (const auto& port : converter->logical_ports) {
        IoPortDefinition definition;
        static_cast<PortContract&>(definition) = port;
        definition.blackboard_key = "input." + port.logical_name;
        definition.logical_name = port.logical_name;
        inputs.push_back(std::move(definition));
      }
    }
    size_t output_index = 0;
    for (const auto& entry : io_descriptor.at("output")) {
      const auto* converter =
          converters.FindOutputConverter(entry.at("type").get<std::string>(),
                                         entry.at("name").get<std::string>());
      ASSERT_NE(converter, nullptr);
      for (const auto& port : converter->logical_ports) {
        IoPortDefinition definition;
        static_cast<PortContract&>(definition) = port;
        definition.logical_name = port.logical_name;
        definition.path =
            "/io/output/" + std::to_string(output_index) + "/inputs";
        const auto bindings = entry.value("inputs", nlohmann::json::object());
        definition.has_binding = bindings.contains(port.logical_name);
        definition.blackboard_key = bindings.value(port.logical_name, "");
        outputs.push_back(std::move(definition));
      }
      ++output_index;
    }
    const auto boundary =
        MakeTestBoundary(std::move(inputs), std::move(outputs));

    // Validator 覆盖完整错误矩阵。
    auto plan = PipelineValidator::ValidateAndPlan(config, boundary);
    EXPECT_FALSE(plan.report.ok);
    ASSERT_FALSE(plan.report.diagnostics.empty());
    const auto json_report = plan.report.ToJson();
    const auto& primary = json_report["diagnostics"].front();
    EXPECT_EQ(primary["code"], test["primary_code"]);
    EXPECT_EQ(primary["path"], test["primary_path"]);
    for (const auto& required_code : test["required_codes"]) {
      EXPECT_TRUE(std::any_of(
          json_report["diagnostics"].begin(), json_report["diagnostics"].end(),
          [&](const auto& item) { return item["code"] == required_code; }))
          << "Missing required diagnostic " << required_code;
    }

    // 结构与参数错误各选一例，验证 Pipeline 和 Resolver 保留主诊断。
    if (name != "missing_node_name" && name != "config_field_range") {
      continue;
    }

    // Pipeline 消费已校验的计划。
    Pipeline pipeline;
    PipelineDiagnostic pipe_diag;
    const bool built = pipeline.BuildFromPlan(
        std::make_unique<ValidatedPipelinePlan>(std::move(plan)), &pipe_diag);
    EXPECT_FALSE(built);
    EXPECT_EQ(pipeline.GetState(), Pipeline::State::kFailed);
    EXPECT_EQ(DiagnosticCodeName(pipe_diag.code),
              test["primary_code"].get<std::string>());
    EXPECT_EQ(pipe_diag.path, test["primary_path"].get<std::string>());
    EXPECT_EQ(pipe_diag.message, primary["message"].get<std::string>());

    // 同一显式 I/O 选择交给 Resolver，在实例化前保留主诊断。
    const auto dep_config =
        MakeSyntheticDeploymentDocForTest(config, io_descriptor);
    std::unique_ptr<ValidatedIoPlan> io_plan;
    std::string resolve_error;
    DeploymentDiagnostic resolve_diagnostic;
    const int resolve_result = IoPlanResolver::ResolveFromPipelineJson(
        dep_config, "", &io_plan, &resolve_error, &resolve_diagnostic);
    EXPECT_EQ(resolve_result, test["runtime_error_code"].get<int>());
    EXPECT_EQ(io_plan, nullptr);
    EXPECT_EQ(resolve_diagnostic.code, test["primary_code"].get<std::string>());
    EXPECT_EQ(resolve_diagnostic.path, test["primary_path"].get<std::string>());
    EXPECT_EQ(resolve_diagnostic.message,
              primary["message"].get<std::string>());
    EXPECT_NE(resolve_error.find(resolve_diagnostic.path), std::string::npos);
  }
}

TEST(PipelineValidatorTest, WhisperPipelineValidationDependsOnBackend) {
  std::ifstream stream("configs/pipeline_audio_asr_intent_cpu.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json pipeline;
  stream >> pipeline;
  auto fixture = PrepareExternalFixtureForCore(pipeline);
  pipeline = fixture.neutral_pipeline_json;
  const auto report = ValidateWithRemediation(pipeline, fixture.io_boundary);
#ifdef HAVE_WHISPERCPP
  EXPECT_TRUE(report.ok) << report.ToJson().dump(2);
  const auto plan =
      PipelineValidator::ValidateAndPlan(pipeline, fixture.io_boundary);
  EXPECT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
#else
  EXPECT_FALSE(report.ok);
  EXPECT_TRUE(std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
                          [](const ValidationDiagnostic& diagnostic) {
                            return diagnostic.code ==
                                       DiagnosticCode::kUnknownBackend &&
                                   diagnostic.path == "/models/0/backend/type";
                          }));
#endif
}

}  // namespace
}  // namespace llm_edgeflow

namespace llm_edgeflow {
TEST(PipelineValidatorTest,
     NestedAndCrossFieldErrorsFailBeforeMaterialization) {
  const std::vector<std::pair<std::string, nlohmann::json>> cases = {
      {"text_chunk", {{"chunk_size", 8}, {"overlap", 8}}},
      {"text_template", {{"template", "{{unknown_variable}}"}}},
      {"text_template", {{"template", "{{unclosed"}}},
      {"structured_json_parse", {{"field_types", {{"x", "unsupported"}}}}},
      {"structured_json_parse", {{"required_fields", {"risk"}}}},
      {"text_corpus_source", {{"corpus", {"valid", 42}}}},
      {"text_rule_match",
       {{"rules", {{{"strategy", "regex"}, {"pattern", "["}}}}}},
  };
  for (const auto& [type, config] : cases) {
    SCOPED_TRACE(type + config.dump());
    std::ifstream stream("configs/pipeline_keyword_match_rules.json");
    nlohmann::json root;
    stream >> root;
    auto fixture = PrepareExternalFixtureForCore(root);
    root = fixture.neutral_pipeline_json;
    root["pipeline"].push_back({{"name", "invalid"},
                                {"type", type},
                                {"depends_on", nlohmann::json::array()},
                                {"params", config}});
    const auto report = ValidateWithRemediation(root, fixture.io_boundary);
    EXPECT_FALSE(report.ok);
    EXPECT_TRUE(
        std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
                    [](const auto& d) {
                      return d.node_name == "invalid" &&
                             (d.code == DiagnosticCode::kInvalidCombination ||
                              d.code == DiagnosticCode::kConfigFieldType ||
                              d.code == DiagnosticCode::kConfigFieldEnum);
                    }))
        << report.ToJson();
    Pipeline pipeline;
    EXPECT_FALSE(BuildTestPipeline(pipeline, root, fixture.io_boundary));
    auto node = NodeRegistry::Instance().Create(type);
    SessionContext session;
    EXPECT_FALSE(InitNodeForTest(*node, config, &session));
  }
}

TEST(PipelineValidatorTest, UnconnectedOptionalPortStaysAbsentAtRuntime) {
  std::ifstream stream("configs/pipeline_keyword_match_rules.json");
  nlohmann::json root;
  stream >> root;
  auto fixture = PrepareExternalFixtureForCore(root);
  root = fixture.neutral_pipeline_json;
  root["pipeline"] =
      nlohmann::json::array({{{"name", "a"},
                              {"type", "text_template"},
                              {"depends_on", nlohmann::json::array()},
                              {"params", {{"template", "UNDECLARED"}}},
                              {"inputs", {{"primary", "input.sentence_text"}}}},
                             {{"name", "b"},
                              {"type", "text_template"},
                              {"depends_on", {"a"}},
                              {"params", {{"template", "{{primary}}|"}}},
                              {"inputs", {{"primary", "input.sentence_text"}}}},
                             {{"name", "rule"},
                              {"type", "text_rule_match"},
                              {"depends_on", {"b"}},
                              {"inputs", {{"text", "b.text"}}}}});
  fixture.io_boundary.output_consumed_ports[0].blackboard_key = "rule.matches";
  const auto plan =
      PipelineValidator::ValidateAndPlan(root, fixture.io_boundary);
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson();
  EXPECT_EQ(plan.node_plans.at("b").FindPort("context"), nullptr);
  Pipeline pipeline;
  ASSERT_TRUE(BuildTestPipeline(pipeline, root, fixture.io_boundary));
  AlgContext ctx;
  ctx.Publish("input.sentence_text", TextBatch{{0, 0, "USER"}});
  ASSERT_EQ(pipeline.Execute(&ctx), 0);
  ASSERT_NE(ctx.Read<TextBatch>("b.text"), nullptr);
  EXPECT_EQ(ctx.Read<TextBatch>("b.text")->at(0).data, "USER|");
}

TEST(PipelineValidatorTest,
     ExplainAcceptsDataConnectionWithoutExplicitDependency) {
  std::ifstream stream(
      "demo/fixtures/mock/pipeline_entity_extract_custom.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json root;
  stream >> root;
  auto fixture = PrepareExternalFixtureForCore(root);
  root = fixture.neutral_pipeline_json;
  for (auto& node : root["pipeline"]) node.erase("depends_on");

  const auto report = ExplainPipeline(root, fixture.io_boundary);
  EXPECT_TRUE(report.ok) << report.ToJson().dump(2);
  EXPECT_TRUE(report.diagnostics.empty());
  const auto plan =
      PipelineValidator::ValidateAndPlan(root, fixture.io_boundary);
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
  ASSERT_EQ(plan.report.topological_order.size(), 2U);
  EXPECT_EQ(plan.report.topological_order.front(), "generate_entities");
}

TEST(PipelineValidatorTest, ExplainReturnsCandidateFixForUnknownConfigField) {
  std::ifstream stream(
      "demo/fixtures/mock/pipeline_entity_extract_custom.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json root;
  stream >> root;
  auto fixture = PrepareExternalFixtureForCore(root);
  root = fixture.neutral_pipeline_json;

  // 把 "temperature" 误拼为 "temprature"
  root["pipeline"][0]["params"]["temprature"] = 0.1;
  root["pipeline"][0]["params"].erase("temperature");

  const auto report = ExplainPipeline(root, fixture.io_boundary);
  EXPECT_FALSE(report.ok);
  ASSERT_FALSE(report.diagnostics.empty());

  const ValidationDiagnostic* target_diag = nullptr;
  for (const auto& diag : report.diagnostics) {
    if (diag.code == DiagnosticCode::kUnknownConfigField &&
        diag.path == "/pipeline/0/params/temprature") {
      target_diag = &diag;
      break;
    }
  }
  ASSERT_NE(target_diag, nullptr);
  ASSERT_TRUE(target_diag->remediation.has_value());
  const auto& rem = *target_diag->remediation;
  EXPECT_EQ(rem.cause, RemediationCause::kUnknownConfigField);
  EXPECT_EQ(rem.facts.value("field", ""), "temprature");

  ASSERT_TRUE(rem.facts.contains("candidate_fields"));
  const auto& candidate_fields = rem.facts["candidate_fields"];
  ASSERT_FALSE(candidate_fields.empty());
  // 最接近的候选 "temperature" (Levenshtein 距离为 1) 应排在首位
  EXPECT_EQ(candidate_fields[0], "temperature");

  ASSERT_FALSE(rem.fixes.empty());
  const auto& fix = rem.fixes.front();
  EXPECT_EQ(fix.verification, "pipeline_valid");

  // 验证补丁把字段从 "temprature" 移到 "temperature"
  bool found_move = false;
  for (const auto& op : fix.patch) {
    if (op.value("op", "") == "move" &&
        op.value("from", "") == "/pipeline/0/params/temprature" &&
        op.value("path", "") == "/pipeline/0/params/temperature") {
      found_move = true;
      break;
    }
  }
  EXPECT_TRUE(found_move);

  // 应用补丁后恢复为完全合法的 Pipeline
  const auto patched = root.patch(fix.patch);
  const auto verified_report =
      ValidateWithRemediation(patched, fixture.io_boundary);
  EXPECT_TRUE(verified_report.ok) << verified_report.ToJson().dump(2);
}

TEST(PipelineValidatorTest, ExplainRespectsFixBounds) {
  const auto boundary =
      MakeTestBoundary({{"input.sentence_text", "TextBatch"}}, {});

  // 构造含 5 个独立上游 Node 和 4 个依赖非法的消费 Node 的 Pipeline。
  // 若不设上限，每个消费者会产生 8 个候选修复，共 32 个。
  // Explain 必须限制每条诊断最多 3 个修复、每份报告最多 8 个修复。
  nlohmann::json root = {{"models", nlohmann::json::array()},
                         {"pipeline", nlohmann::json::array()}};

  for (int i = 0; i < 5; ++i) {
    root["pipeline"].push_back({{"name", "upstream_" + std::to_string(i)},
                                {"type", "text_corpus_source"},
                                {"depends_on", nlohmann::json::array()},
                                {"params", {{"corpus", {"sample"}}}}});
  }

  for (int i = 0; i < 4; ++i) {
    root["pipeline"].push_back({{"name", "consumer_" + std::to_string(i)},
                                {"type", "text_corpus_source"},
                                {"depends_on", {"nonexistent_node"}},
                                {"params", {{"corpus", {"sample"}}}}});
  }

  const auto report = ExplainPipeline(root, boundary);
  EXPECT_FALSE(report.ok);

  size_t total_fixes = 0;
  bool saw_max_per_diag = false;
  for (const auto& diag : report.diagnostics) {
    if (diag.remediation.has_value()) {
      EXPECT_LE(diag.remediation->fixes.size(), 3U);
      if (diag.remediation->fixes.size() == 3U) {
        saw_max_per_diag = true;
      }
      total_fixes += diag.remediation->fixes.size();
    }
  }

  EXPECT_TRUE(saw_max_per_diag);
  EXPECT_EQ(total_fixes, 8U);
}

TEST(PipelineValidatorTest, ExplainCleanPipelineReturnsOk) {
  std::ifstream stream("configs/pipeline_keyword_match_rules.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json root;
  stream >> root;
  auto fixture = PrepareExternalFixtureForCore(root);
  root = fixture.neutral_pipeline_json;

  const auto report = ExplainPipeline(root, fixture.io_boundary);
  EXPECT_TRUE(report.ok);
  EXPECT_TRUE(report.diagnostics.empty());
  EXPECT_FALSE(report.topological_order.empty());
}

TEST(PipelineValidatorTest, ExplainTargetResolved) {
  std::ifstream stream(
      "demo/fixtures/mock/pipeline_entity_extract_custom.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json root;
  stream >> root;
  auto fixture = PrepareExternalFixtureForCore(root);
  root = fixture.neutral_pipeline_json;

  // 引入两个相互独立的错误：
  // 1. custom_prompt 中的配置字段拼写错误 ("temprature" 而非 "temperature")
  root["pipeline"][0]["params"]["temprature"] = 0.1;
  root["pipeline"][0]["params"].erase("temperature");

  // 2. parse_entities 中仍未解决的非法配置
  root["pipeline"][1]["params"]["field_types"] = {
      {"field_x", "unsupported_type"}};

  const auto report = ExplainPipeline(root, fixture.io_boundary);
  EXPECT_FALSE(report.ok);
  ASSERT_GE(report.diagnostics.size(), 2U);

  const ValidationDiagnostic* typo_diag = nullptr;
  for (const auto& diag : report.diagnostics) {
    if (diag.code == DiagnosticCode::kUnknownConfigField &&
        diag.path == "/pipeline/0/params/temprature") {
      typo_diag = &diag;
      break;
    }
  }

  ASSERT_NE(typo_diag, nullptr);
  ASSERT_TRUE(typo_diag->remediation.has_value());
  ASSERT_FALSE(typo_diag->remediation->fixes.empty());

  // 应用该修复只解决了未知配置字段，node 1 中非法的 field_types
  // 错误仍然存在，因此 verification 必须为 "target_resolved"。
  const auto& fix = typo_diag->remediation->fixes.front();
  EXPECT_EQ(fix.verification, "target_resolved");
}

TEST(PipelineValidatorTest, ValidateProducesBasicRemediation) {
  std::ifstream stream(
      "demo/fixtures/mock/pipeline_entity_extract_custom.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json root;
  stream >> root;
  auto fixture = PrepareExternalFixtureForCore(root);
  root = fixture.neutral_pipeline_json;

  // 显式把输入连接到没有生产者的键。
  root["pipeline"][1]["inputs"]["text"] = "missing_result.output";

  const auto report = ValidateWithRemediation(root, fixture.io_boundary);
  EXPECT_FALSE(report.ok);
  ASSERT_FALSE(report.diagnostics.empty());

  const ValidationDiagnostic* target_diag = nullptr;
  for (const auto& diag : report.diagnostics) {
    if (diag.code == DiagnosticCode::kUnknownNodeReference &&
        diag.node_name == "parse_entities" && diag.port == "text") {
      target_diag = &diag;
      break;
    }
  }
  ASSERT_NE(target_diag, nullptr);
  ASSERT_TRUE(target_diag->remediation.has_value());
  EXPECT_NE(RemediationCauseName(target_diag->remediation->cause), "UNKNOWN");
  EXPECT_FALSE(target_diag->remediation->summary.empty());
  EXPECT_FALSE(target_diag->remediation->facts.empty());
  EXPECT_EQ(target_diag->remediation->cause,
            RemediationCause::kUnknownNodeReference);
  EXPECT_EQ(target_diag->remediation->facts.at("bound_key"),
            "missing_result.output");
  EXPECT_TRUE(target_diag->remediation->fixes.empty());
}

TEST(PipelineValidatorTest, ExplainCapsVerificationAttemptsAtEight) {
  const auto boundary = MakeTestBoundary(
      {{"input.sentence_text", "TextBatch"}},
      {{"parse_entities.document", "StructuredDocumentBatch"}});

  nlohmann::json root = {{"models", nlohmann::json::array()},
                         {"pipeline",
                          {{{"name", "custom_prompt"},
                            {"type", "prompt_guided_llm"},
                            {"depends_on", nlohmann::json::array()},
                            {"inputs", {{"input", "input.sentence_text"}}},
                            {"params",
                             {{"bind_model", "missing_model"},
                              {"prompt_template", "Hello {{input}}"},
                              {"strip_markdown", true},
                              {"temperature", 0.1},
                              {"max_tokens", 64}}}},
                           {{"name", "parse_entities"},
                            {"type", "structured_json_parse"},
                            {"depends_on", {"custom_prompt"}},
                            {"inputs", {{"text", "custom_prompt.output"}}},
                            {"params", {{"failure_policy", "fail"}}}}}}};

  for (int i = 0; i < 10; ++i) {
    char name_buf[32];
    std::snprintf(name_buf, sizeof(name_buf), "a_invalid_%02d", i);
    root["models"].push_back({
        {"name", name_buf},
        {"type", "llm"},
        {"backend",
         {{"type", "unknown_backend"}, {"params", {{"fixed_batch_size", 2}}}}},
        {"file", "demo/fixtures/mock/artifacts/neutral-llm.fixture"},
        {"params", nlohmann::json::object()},
    });
  }

  root["models"].push_back({
      {"name", "z_valid"},
      {"type", "llm"},
      {"backend",
       {{"type", "test_causal_lm_backend"},
        {"params", {{"fixed_batch_size", 2}}}}},
      {"file", "demo/fixtures/mock/artifacts/neutral-llm.fixture"},
      {"params", nlohmann::json::object()},
  });

  const auto report = ExplainPipeline(root, boundary);
  EXPECT_FALSE(report.ok);

  size_t total_fixes = 0;
  bool found_model_11_or_z_valid = false;
  for (const auto& diag : report.diagnostics) {
    if (diag.remediation.has_value()) {
      total_fixes += diag.remediation->fixes.size();
      for (const auto& fix : diag.remediation->fixes) {
        if (fix.id == "use-model-11" ||
            fix.title.find("z_valid") != std::string::npos) {
          found_model_11_or_z_valid = true;
        }
      }
    }
  }

  EXPECT_LE(total_fixes, 8U);
  EXPECT_FALSE(found_model_11_or_z_valid);

  // Removing the invalid candidates makes the same final candidate usable.
  // Its patch must repair the configuration, proving the bound above is
  // exercised.
  auto reachable = root;
  reachable["models"] = nlohmann::json::array({root["models"].back()});
  const auto reachable_report = ExplainPipeline(reachable, boundary);
  const auto* reference =
      FindDiagnostic(reachable_report, DiagnosticCode::kUnknownModelReference,
                     "/pipeline/0/params/bind_model");
  ASSERT_NE(reference, nullptr);
  ASSERT_TRUE(reference->remediation.has_value());
  ASSERT_FALSE(reference->remediation->fixes.empty());
  const auto repaired =
      reachable.patch(reference->remediation->fixes.front().patch);
  const auto repaired_report = ValidateWithRemediation(repaired, boundary);
  EXPECT_TRUE(repaired_report.ok) << repaired_report.ToJson().dump(2);
}

TEST(PipelineValidatorTest, ExplainRejectsInvalidModelCandidates) {
  const auto boundary =
      MakeTestBoundary({{"input.sentence_text", "TextBatch"}}, {});

  nlohmann::json root = {{"models",
                          {{{"name", "broken_model"},
                            {"type", "llm"},
                            {"backend",
                             {{"type", "test_causal_lm_backend"},
                              {"params", {{"fixed_batch_size", 2}}}}},
                            {"file", "fixture.bin"},
                            {"params", {{"max_seq_len", 0}}}}}},
                         {"pipeline",
                          {{{"name", "llm_node"},
                            {"type", "prompt_guided_llm"},
                            {"depends_on", nlohmann::json::array()},
                            {"inputs", {{"input", "input.sentence_text"}}},
                            {"params",
                             {{"bind_model", "missing_model"},
                              {"prompt_template", "Hello {{input}}"},
                              {"strip_markdown", true},
                              {"temperature", 0.1},
                              {"max_tokens", 64}}}}}}};

  const auto report = ExplainPipeline(root, boundary);
  EXPECT_FALSE(report.ok);

  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kConfigFieldRange,
                           "/models/0/params/max_seq_len"),
            nullptr);
  const auto* reference =
      FindDiagnostic(report, DiagnosticCode::kUnknownModelReference,
                     "/pipeline/0/params/bind_model");
  ASSERT_NE(reference, nullptr);
  ASSERT_TRUE(reference->remediation.has_value());
  EXPECT_EQ(reference->remediation->facts["candidate_model_names"],
            nlohmann::json::array({"broken_model"}));

  bool proposed_broken_model = false;
  for (const auto& diag : report.diagnostics) {
    if (diag.remediation.has_value()) {
      for (const auto& fix : diag.remediation->fixes) {
        if (fix.title.find("broken_model") != std::string::npos ||
            fix.id.find("broken_model") != std::string::npos) {
          proposed_broken_model = true;
        }
        for (const auto& op : fix.patch) {
          if (op.value("value", "") == "broken_model") {
            proposed_broken_model = true;
          }
        }
      }
    }
  }
  EXPECT_FALSE(proposed_broken_model);
}

TEST(PipelineValidatorTest, IoTypeMismatchSuggestsAnotherOutputOfSameNode) {
  auto fixture = LoadRegistrationFixture(
      "demo/fixtures/mock/pipeline_entity_extract.json");
  const auto& nodes = fixture.neutral_pipeline_json["pipeline"];
  const auto generator =
      std::find_if(nodes.begin(), nodes.end(), [](const auto& node) {
        return node.value("type", "") == "llm_generate";
      });
  ASSERT_NE(generator, nodes.end());
  auto node = *generator;
  const std::string name = node.at("name").get<std::string>();
  node["inputs"]["input"] = "input.sentence_text";
  fixture.neutral_pipeline_json["pipeline"] = nlohmann::json::array({node});
  ASSERT_EQ(fixture.io_boundary.output_consumed_ports.size(), 1U);
  auto& output = fixture.io_boundary.output_consumed_ports[0];
  output.blackboard_key = name + ".text";

  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  ASSERT_FALSE(report.ok);
  ASSERT_EQ(report.diagnostics.size(), 1U) << report.ToJson();
  const auto& diagnostic = report.diagnostics[0];
  EXPECT_EQ(diagnostic.code, DiagnosticCode::kPortTypeMismatch);
  EXPECT_EQ(diagnostic.path, "/io/output/0/inputs/entities");
  EXPECT_EQ(diagnostic.node_name, name);
  EXPECT_EQ(diagnostic.port, "entities");
  ASSERT_TRUE(diagnostic.remediation.has_value());
  const auto& remediation = *diagnostic.remediation;
  EXPECT_EQ(remediation.cause, RemediationCause::kPortTypeMismatch);
  EXPECT_EQ(remediation.facts.at("bound_key"), name + ".text");
  EXPECT_EQ(remediation.facts.at("expected_type"), "StructuredDocumentBatch");
  const auto candidates =
      remediation.facts.at("candidate_sources").get<std::vector<std::string>>();
  EXPECT_EQ(candidates, std::vector<std::string>{name + ".document"});

  output.blackboard_key = name + ".document";
  const auto repaired = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                                fixture.io_boundary);
  EXPECT_TRUE(repaired.ok) << repaired.ToJson();
}

TEST(PipelineValidatorTest, SplitItemsFeedItemWiseNodeButNotPerRequestEgress) {
  auto boundary =
      MakeTestBoundary({{"input.sentence_text", "TextBatch"}},
                       {{"rules_per_request.matches", "RuleMatchBatch"}});

  nlohmann::json root = {
      {"pipeline",
       {{{"name", "chunk"},
         {"type", "text_chunk"},
         {"params", {{"chunk_size", 4}}},
         {"inputs", {{"text", "input.sentence_text"}}}},
        {{"name", "rules_per_chunk"},
         {"type", "text_rule_match"},
         {"params", {{"categories", {{"SYSTEM_INIT", {"初始化"}}}}}},
         {"inputs", {{"text", "chunk.chunks"}}}},
        {{"name", "rules_per_request"},
         {"type", "text_rule_match"},
         {"params", {{"categories", {{"SYSTEM_INIT", {"初始化"}}}}}},
         {"inputs", {{"text", "input.sentence_text"}}}}}}};

  boundary.output_consumed_ports[0].logical_name = "matches";
  boundary.output_consumed_ports[0].path = "/io/output/0/inputs";
  const auto accepted = ValidateWithRemediation(root, boundary);
  EXPECT_TRUE(accepted.ok) << accepted.ToJson().dump();

  // 逐分块的匹配结果不能作为每请求一行的 I/O 输出。
  boundary.output_consumed_ports[0].blackboard_key = "rules_per_chunk.matches";
  const auto rejected = ValidateWithRemediation(root, boundary);
  ASSERT_FALSE(rejected.ok);
  ASSERT_EQ(rejected.diagnostics.size(), 1u) << rejected.ToJson().dump();
  EXPECT_EQ(rejected.diagnostics[0].code,
            DiagnosticCode::kPortCardinalityMismatch);
  EXPECT_EQ(rejected.diagnostics[0].path, "/io/output/0/inputs/matches");
  EXPECT_EQ(rejected.diagnostics[0].port, "matches");
  const auto& egress = rejected.diagnostics[0];
  ASSERT_TRUE(egress.remediation.has_value());
  EXPECT_EQ(egress.remediation->cause, RemediationCause::kPortFlowMismatch);
  EXPECT_NE(egress.remediation->summary.find("生产者 'rules_per_chunk'"),
            std::string::npos);
  EXPECT_NE(egress.remediation->summary.find("消费者 'output'"),
            std::string::npos);
  EXPECT_NE(egress.remediation->summary.find("chunk.chunks"),
            std::string::npos);
  EXPECT_NE(egress.remediation->summary.find("每请求一项"), std::string::npos);
  const nlohmann::json declaration = {{"type_id", "RuleMatchBatch"},
                                      {"cardinality", "1:1"},
                                      {"provenance_policy", "preserve"},
                                      {"lifetime", "request"}};
  const nlohmann::json actual_shape = {{"kind", "multi"},
                                       {"origin", "chunk.chunks"}};
  const nlohmann::json expected_shape = {{"kind", "per_request"}};
  const auto& facts = egress.remediation->facts;
  EXPECT_EQ(facts.at("producer_name"), "rules_per_chunk");
  EXPECT_EQ(facts.at("consumer_name"), "output");
  EXPECT_EQ(facts.at("bound_key"), "rules_per_chunk.matches");
  EXPECT_EQ(facts.at("actual"), declaration);
  EXPECT_EQ(facts.at("expected"), declaration);
  EXPECT_EQ(facts.at("actual_shape"), actual_shape);
  EXPECT_EQ(facts.at("expected_shape"), expected_shape);
  EXPECT_EQ(egress.facts, facts);
  EXPECT_EQ(egress.ToJson().at("facts"), facts);
}

TEST(PipelineValidatorTest, ExplainReturnsPortFlowMismatchRemediation) {
  test_support::RegistryTestAccess::ScopedNodeState state;
  NodeDefinition consumer_definition;
  consumer_definition.node_type = "session_text_consumer_fixture";
  consumer_definition.category = "test";
  consumer_definition.inputs = {
      {"flow", "TextBatch", true, "1:1", "preserve", "session"}};
  ASSERT_TRUE(NodeRegistry::Instance().Register(
      consumer_definition.node_type,
      []() -> std::unique_ptr<INode> { return nullptr; }, consumer_definition));
  const auto boundary =
      MakeTestBoundary({{"input.sentence_text", "TextBatch"}}, {});

  nlohmann::json root = {{"models", nlohmann::json::array()},
                         {"pipeline",
                          {{{"name", "producer"},
                            {"type", "text_chunk"},
                            {"depends_on", nlohmann::json::array()},
                            {"inputs", {{"text", "input.sentence_text"}}}},
                           {{"name", "consumer"},
                            {"type", consumer_definition.node_type},
                            {"depends_on", {"producer"}},
                            {"inputs", {{"flow", "producer.chunks"}}}}}}};

  const auto report = ValidateWithRemediation(root, boundary);
  EXPECT_FALSE(report.ok);
  ASSERT_FALSE(report.diagnostics.empty());

  // 请求作用域的分块不能连接要求会话作用域的消费端口。
  const ValidationDiagnostic* target_diag = nullptr;
  for (const auto& diag : report.diagnostics) {
    if (diag.code == DiagnosticCode::kPortLifetimeMismatch &&
        diag.node_name == "consumer" && diag.port == "flow") {
      target_diag = &diag;
      break;
    }
  }

  ASSERT_NE(target_diag, nullptr) << report.ToJson();
  ASSERT_TRUE(target_diag->remediation.has_value());
  EXPECT_EQ(target_diag->remediation->cause,
            RemediationCause::kPortFlowMismatch);
  EXPECT_EQ(target_diag->remediation->facts.value("bound_key", ""),
            "producer.chunks");
  EXPECT_EQ(target_diag->remediation->facts.value("producer_name", ""),
            "producer");

  ASSERT_TRUE(target_diag->remediation->facts.contains("expected"));
  ASSERT_TRUE(target_diag->remediation->facts.contains("actual"));
  const auto& expected = target_diag->remediation->facts["expected"];
  const auto& actual = target_diag->remediation->facts["actual"];

  EXPECT_TRUE(expected.contains("cardinality"));
  EXPECT_TRUE(expected.contains("provenance_policy"));
  EXPECT_TRUE(expected.contains("lifetime"));

  EXPECT_TRUE(actual.contains("cardinality"));
  EXPECT_TRUE(actual.contains("provenance_policy"));
  EXPECT_TRUE(actual.contains("lifetime"));
  EXPECT_EQ(target_diag->remediation->facts.at("consumer_name"), "consumer");
  EXPECT_EQ(expected.at("type_id"), "TextBatch");
  EXPECT_EQ(actual.at("type_id"), "TextBatch");
  EXPECT_EQ(expected.at("lifetime"), "session");
  EXPECT_EQ(actual.at("lifetime"), "request");
  EXPECT_EQ(expected.at("cardinality"), "1:1");
  EXPECT_EQ(actual.at("cardinality"), "1:N");
  EXPECT_EQ(target_diag->facts, target_diag->remediation->facts);
}

TEST(PipelineValidatorTest,
     ExplainHandlesMultipleDifferentDuplicateDependencies) {
  const auto boundary =
      MakeTestBoundary({{"input.sentence_text", "TextBatch"}}, {});

  nlohmann::json root = {
      {"models", nlohmann::json::array()},
      {"pipeline",
       {{{"name", "dep_a"},
         {"type", "text_corpus_source"},
         {"depends_on", nlohmann::json::array()},
         {"params", {{"corpus", {"a"}}}}},
        {{"name", "dep_b"},
         {"type", "text_corpus_source"},
         {"depends_on", nlohmann::json::array()},
         {"params", {{"corpus", {"b"}}}}},
        {{"name", "consumer"},
         {"type", "text_corpus_source"},
         {"depends_on", {"dep_a", "dep_a", "dep_b", "dep_b"}},
         {"params", {{"corpus", {"c"}}}}}}}};

  const auto report = ExplainPipeline(root, boundary);
  EXPECT_FALSE(report.ok);

  const ValidationDiagnostic* diag_a = nullptr;
  const ValidationDiagnostic* diag_b = nullptr;

  for (const auto& diag : report.diagnostics) {
    if (diag.code == DiagnosticCode::kDuplicateDependency) {
      if (diag.path == "/pipeline/2/depends_on/1") {
        diag_a = &diag;
      } else if (diag.path == "/pipeline/2/depends_on/3") {
        diag_b = &diag;
      }
    }
  }

  ASSERT_NE(diag_a, nullptr);
  ASSERT_TRUE(diag_a->remediation.has_value());
  EXPECT_EQ(diag_a->remediation->facts.value("dependency_name", ""), "dep_a");
  ASSERT_FALSE(diag_a->remediation->fixes.empty());
  bool found_remove_a = false;
  for (const auto& fix : diag_a->remediation->fixes) {
    for (const auto& op : fix.patch) {
      if (op.value("op", "") == "remove" &&
          op.value("path", "") == "/pipeline/2/depends_on/1") {
        found_remove_a = true;
      }
    }
  }
  EXPECT_TRUE(found_remove_a);

  ASSERT_NE(diag_b, nullptr);
  ASSERT_TRUE(diag_b->remediation.has_value());
  EXPECT_EQ(diag_b->remediation->facts.value("dependency_name", ""), "dep_b");
  ASSERT_FALSE(diag_b->remediation->fixes.empty());
  bool found_remove_b = false;
  for (const auto& fix : diag_b->remediation->fixes) {
    for (const auto& op : fix.patch) {
      if (op.value("op", "") == "remove" &&
          op.value("path", "") == "/pipeline/2/depends_on/3") {
        found_remove_b = true;
      }
    }
  }
  EXPECT_TRUE(found_remove_b);
}

}  // namespace llm_edgeflow

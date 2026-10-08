#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "adapter/biz_blackboard_keys.h"
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

namespace llm_edgeflow {
namespace {

class StudioCatalogProbeNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "StudioCatalogProbeNode";
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

// 接入层拆出 io：返回交给 Core 的文档，边界由所选 converter 组成。
nlohmann::json PrepareExternalFixtureForCore(const nlohmann::json& document,
                                             PipelineIoBoundary* boundary) {
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  EXPECT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
      << diagnostic.code << " " << diagnostic.path << " " << diagnostic.message;
  *boundary = prepared.io_boundary;
  return prepared.neutral_pipeline_json;
}

// 实体抽取的 IO 边界：输入句子，输出抽取出的实体文档。
PipelineIoBoundary EntityExtractTestBoundary() {
  return MakeTestBoundary(
      {IoPortDefinition{kInputSentences.name, kInputSentences.type_id, true}},
      {IoPortDefinition{kExtractedEntities.name, kExtractedEntities.type_id,
                        true}});
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
  unknown["id"] = "unknown";
  unknown["node_type"] = "TextRuleMatchNod";
  unknown["outputs"]["matches"] = "shared_key";
  pipeline.push_back(unknown);
  for (size_t i = 0; i < known_producers; ++i) {
    auto known = prototype;
    known["id"] = "known_" + std::to_string(i);
    known["outputs"]["matches"] = "shared_key";
    pipeline.push_back(known);
  }
  auto consumer = prototype;
  consumer["id"] = "consumer";
  consumer["inputs"]["text"] = "shared_key";
  consumer["outputs"]["matches"] = "consumer_matches";
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
  fixture.neutral_pipeline_json["models"][0]["model_type"] = "test_biz_llmm";
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  ASSERT_EQ(report.diagnostics.size(), 1u) << report.ToJson().dump(2);
  const auto* d = FindDiagnostic(report, DiagnosticCode::kUnknownModelType);
  ASSERT_NE(d, nullptr);
  ASSERT_TRUE(d->remediation.has_value());
  EXPECT_EQ(d->remediation->cause, RemediationCause::kUnknownModelType);
  EXPECT_EQ(d->message, "Unknown model_type: test_biz_llmm");
  ASSERT_FALSE(d->suggestions.empty());
  EXPECT_EQ(d->suggestions.front(), "test_biz_llm");
  EXPECT_EQ(d->remediation->facts["candidate_model_types"], d->suggestions);
  const auto names = d->remediation->facts["registered_model_types"]
                         .get<std::vector<std::string>>();
  EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
  EXPECT_EQ(d->remediation->facts["model_id"], "entity_llm");
  EXPECT_EQ(FindDiagnostic(report, DiagnosticCode::kUnknownModelReference),
            nullptr);
  EXPECT_EQ(d->remediation->summary.find("alg_pipeline_tool"),
            std::string::npos);
}

TEST(PipelineValidatorTest, UndeclaredModelReferenceIsStillReported) {
  auto fixture = LoadRegistrationFixture(
      "demo/fixtures/mock/pipeline_entity_extract_custom.json");
  fixture.neutral_pipeline_json["pipeline"][0]["config"]["bind_model"] =
      "undeclared";
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kUnknownModelReference,
                           "/pipeline/0/config/bind_model"),
            nullptr);
}

TEST(PipelineValidatorTest, UnknownBackendHasRemediation) {
  auto fixture = LoadRegistrationFixture(
      "demo/fixtures/mock/pipeline_entity_extract_custom.json");
  fixture.neutral_pipeline_json["models"][0]["backend"] =
      "test_causal_lm_backnd";
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  ASSERT_EQ(report.diagnostics.size(), 1u) << report.ToJson().dump(2);
  const auto& d = report.diagnostics.front();
  EXPECT_EQ(d.code, DiagnosticCode::kUnknownBackend);
  ASSERT_TRUE(d.remediation.has_value());
  EXPECT_EQ(d.remediation->cause, RemediationCause::kUnknownBackend);
  ASSERT_FALSE(d.suggestions.empty());
  EXPECT_EQ(d.suggestions.front(), "test_causal_lm_backend");
  EXPECT_EQ(d.remediation->facts["candidate_backends"], d.suggestions);
  const auto names = d.remediation->facts["registered_backends"]
                         .get<std::vector<std::string>>();
  ASSERT_FALSE(names.empty());
  EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
}

TEST(PipelineValidatorTest, UnknownNodeTypeSuppressesMissingOutputCascade) {
  auto fixture =
      LoadRegistrationFixture("configs/pipeline_keyword_match_rules.json");
  fixture.neutral_pipeline_json["pipeline"][0]["node_type"] =
      "TextRuleMatchNod";
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  ASSERT_EQ(report.diagnostics.size(), 1u) << report.ToJson().dump(2);
  const auto* d = FindDiagnostic(report, DiagnosticCode::kUnknownNodeType);
  ASSERT_NE(d, nullptr);
  ASSERT_TRUE(d->remediation.has_value());
  EXPECT_EQ(d->remediation->cause, RemediationCause::kUnknownNodeType);
  EXPECT_EQ(d->suggestions, std::vector<std::string>{"TextRuleMatchNode"});
  EXPECT_EQ(d->remediation->facts["candidate_node_types"], d->suggestions);
  EXPECT_NE(d->remediation->summary.find("重新构建"), std::string::npos);
}

TEST(PipelineValidatorTest, DistantUnknownNamesHaveNoSuggestions) {
  auto fixture =
      LoadRegistrationFixture("configs/pipeline_keyword_match_rules.json");
  fixture.neutral_pipeline_json["pipeline"][0]["node_type"] =
      "ZZZZZZZZZZZZZZZZZZZZ";
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
      {{"id", "probe"}, {"node_type", "StudioCatalogProbeNode"}}};
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  const auto* d =
      FindDiagnostic(report, DiagnosticCode::kMissingOutputProducer);
  ASSERT_NE(d, nullptr) << report.ToJson().dump(2);
  EXPECT_EQ(d->path, "/io/output");
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
  auto& config = fixture.neutral_pipeline_json["pipeline"][0]["config"];
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
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kMissingInputProducer,
                           "/pipeline/3/inputs/text"),
            nullptr);
}

TEST(PipelineValidatorTest, UnknownNodeDoesNotHideDuplicateProducers) {
  auto fixture = UnknownProducerFixture(2);
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  ASSERT_EQ(report.diagnostics.size(), 3u) << report.ToJson().dump(2);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kUnknownNodeType), nullptr);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kDuplicatePortProducer,
                           "/pipeline/3/outputs/matches"),
            nullptr);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kMissingInputProducer,
                           "/pipeline/4/inputs/text"),
            nullptr);
}

TEST(PipelineValidatorTest, UnknownNodeDoesNotHideIngressTypeMismatch) {
  auto fixture = UnknownProducerFixture();
  auto& pipeline = fixture.neutral_pipeline_json["pipeline"];
  pipeline[1]["outputs"]["matches"] = "input_sentences";
  pipeline[2]["node_type"] = "VectorTopKNode";
  pipeline[2]["config"] = nlohmann::json::object();
  pipeline[2]["inputs"] = {{"queries", "input_sentences"},
                           {"candidates", "input_sentences"}};
  pipeline[2]["outputs"] = nlohmann::json::object();
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kUnknownNodeType), nullptr);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kMissingInputProducer,
                           "/pipeline/2/inputs/queries"),
            nullptr)
      << report.ToJson().dump(2);
}

TEST(PipelineValidatorTest,
     UnknownNodeDoesNotHideKnownProducerIngressConflict) {
  auto fixture = UnknownProducerFixture(1);
  auto& pipeline = fixture.neutral_pipeline_json["pipeline"];
  pipeline[1]["outputs"]["matches"] = "input_sentences";
  pipeline[2]["outputs"]["matches"] = "input_sentences";
  pipeline[3]["inputs"]["text"] = "input_sentences";
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  ASSERT_EQ(report.diagnostics.size(), 5u) << report.ToJson().dump(2);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kUnknownNodeType), nullptr);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kDuplicatePortProducer,
                           "/pipeline/2/outputs/matches"),
            nullptr);
  for (const auto* path : {"/pipeline/0/inputs/text", "/pipeline/2/inputs/text",
                           "/pipeline/3/inputs/text"}) {
    EXPECT_NE(
        FindDiagnostic(report, DiagnosticCode::kMissingInputProducer, path),
        nullptr)
        << path;
  }
}

TEST(PipelineValidatorTest, UnknownNodeDoesNotHideUnrelatedMissingInput) {
  auto fixture = UnknownProducerFixture();
  fixture.neutral_pipeline_json["pipeline"][2]["inputs"]["text"] =
      "unrelated_key";
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  ASSERT_EQ(report.diagnostics.size(), 2u) << report.ToJson().dump(2);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kMissingInputProducer,
                           "/pipeline/2/inputs/text"),
            nullptr);
}

TEST(PipelineValidatorTest,
     UnknownNodeWithoutOutputMappingDoesNotHideMissingOutput) {
  auto fixture =
      LoadRegistrationFixture("configs/pipeline_keyword_match_rules.json");
  fixture.neutral_pipeline_json["pipeline"][0]["node_type"] =
      "TextRuleMatchNod";
  fixture.neutral_pipeline_json["pipeline"][0]["outputs"] =
      nlohmann::json::object();
  const auto report = ValidateWithRemediation(fixture.neutral_pipeline_json,
                                              fixture.io_boundary);
  ASSERT_EQ(report.diagnostics.size(), 2u) << report.ToJson().dump(2);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kMissingOutputProducer,
                           "/io/output"),
            nullptr);
}

TEST(PipelineValidatorTest, UnknownModelTypeDoesNotHideCapabilityMismatch) {
  auto fixture = LoadRegistrationFixture(
      "demo/fixtures/mock/pipeline_entity_extract_custom.json");
  auto& root = fixture.neutral_pipeline_json;
  root["models"][0]["model_type"] = "test_biz_llmm";
  root["models"].push_back({{"model_id", "known_embedding"},
                            {"model_type", "test_biz_embedding"},
                            {"backend", "test_tensor_backend"},
                            {"model_path", "fixture.bin"},
                            {"model_config", nlohmann::json::object()},
                            {"backend_config", nlohmann::json::object()}});
  root["pipeline"][0]["config"]["bind_model"] = "known_embedding";
  const auto report = ValidateWithRemediation(root, fixture.io_boundary);
  ASSERT_EQ(report.diagnostics.size(), 2u) << report.ToJson().dump(2);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kUnknownModelType), nullptr);
  EXPECT_NE(FindDiagnostic(report, DiagnosticCode::kModelCapabilityMismatch,
                           "/pipeline/0/config/bind_model"),
            nullptr);
}

TEST(PipelineCatalogTest, RegisteredProductionTypesHaveDefinitions) {
  for (const auto& node_type : NodeRegistry::Instance().ListTypes()) {
    EXPECT_TRUE(PipelineCatalog::FindNode(node_type).has_value()) << node_type;
  }
  for (const auto& model_type : ModelRegistry::Instance().ListTypes()) {
    EXPECT_TRUE(PipelineCatalog::FindModel(model_type).has_value())
        << model_type;
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
    EXPECT_TRUE(model_types.insert(definition.model_type).second)
        << definition.model_type;
  }
  std::set<std::string> backend_types;
  for (const auto& definition : PipelineCatalog::Backends()) {
    EXPECT_TRUE(backend_types.insert(definition.backend_type).second)
        << definition.backend_type;
  }
}

TEST(PipelineCatalogTest, DefinitionRegistrationMakesNewNodeDiscoverable) {
  const auto definition = PipelineCatalog::FindNode("StudioCatalogProbeNode");
  ASSERT_TRUE(definition.has_value());
  EXPECT_EQ(definition->description, "Catalog auto-discovery probe");
  const auto filtered = PipelineCatalog::ToJson();
  EXPECT_TRUE(std::any_of(
      filtered["nodes"].begin(), filtered["nodes"].end(), [](const auto& item) {
        return item["node_type"] == "StudioCatalogProbeNode";
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
      const std::string backend = model.value("backend", "");
      const std::string model_type = model.value("model_type", "");
      if ((!backend.empty() && !BackendRegistry::Instance().Has(backend)) ||
          (!model_type.empty() && !ModelRegistry::Instance().Has(model_type))) {
        requires_unavailable_runtime = true;
        break;
      }
    }
    if (requires_unavailable_runtime) {
      ++skipped_optional;
      continue;
    }
    // io 由接入层独占解析，Core 直接看到它时按未知根字段拒绝。
    const auto direct_report =
        ValidateWithRemediation(pipeline, EmptyTestBoundary());
    EXPECT_FALSE(direct_report.ok);
    EXPECT_TRUE(std::any_of(
        direct_report.diagnostics.begin(), direct_report.diagnostics.end(),
        [](const auto& diagnostic) {
          return diagnostic.code == DiagnosticCode::kUnknownField &&
                 diagnostic.path == "/io";
        }));
    PipelineIoBoundary boundary;
    const auto neutral = PrepareExternalFixtureForCore(pipeline, &boundary);
    const auto report = ValidateWithRemediation(neutral, boundary);
    EXPECT_TRUE(report.ok) << entry.path() << "\n" << report.ToJson().dump(2);
    ++validated;
  }
  EXPECT_GT(validated, 0U);
  EXPECT_EQ(validated + skipped_optional, candidates);
}

TEST(PipelineValidatorTest, ModelPathsUseLexicalChecksWithoutDeploymentRoots) {
  std::ifstream stream("demo/fixtures/mock/pipeline_doc_qa.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json pipeline;
  ASSERT_NO_THROW(stream >> pipeline);
  PipelineIoBoundary boundary;
  pipeline = PrepareExternalFixtureForCore(pipeline, &boundary);

  for (const std::string& safe_path :
       {std::string("missing/artifact.bin"), std::string("..name/artifact.bin"),
        std::string("missing/../artifact.bin"),
        std::filesystem::absolute("missing/artifact.bin").string()}) {
    pipeline["models"][0]["model_path"] = safe_path;
    const auto report = ValidateWithRemediation(pipeline, boundary);
    EXPECT_TRUE(report.ok) << safe_path << "\n" << report.ToJson().dump(2);
  }

  for (const char* unsafe_path :
       {"..", "../artifact.bin", "missing/../../artifact.bin",
        "..\\artifact.bin"}) {
    pipeline["models"][0]["model_path"] = unsafe_path;
    const auto report = ValidateWithRemediation(pipeline, boundary);
    EXPECT_FALSE(report.ok) << unsafe_path;
    EXPECT_TRUE(
        std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
                    [](const ValidationDiagnostic& diagnostic) {
                      return diagnostic.code == DiagnosticCode::kFieldRange &&
                             diagnostic.path == "/models/0/model_path";
                    }))
        << unsafe_path << "\n"
        << report.ToJson().dump(2);
  }
}

TEST(PipelineValidatorTest, ReportsConfigAndCapabilityErrors) {
  const nlohmann::json pipeline = {
      {"models",
       {{{"model_id", "llm_model"},
         {"model_type", "test_biz_embedding"},
         {"backend", "test_tensor_backend"},
         {"model_path", "fixture.bin"},
         {"model_config", nlohmann::json::object()},
         {"backend_config", nlohmann::json::object()}}}},
      {"pipeline",
       {{{"id", "pre"},
         {"node_type", "TextTemplateNode"},
         {"depends_on", nlohmann::json::array()}},
        {{"id", "llm"},
         {"node_type", "LlmGenerateNode"},
         {"depends_on", {"pre"}},
         {"config",
          {{"bind_model", "llm_model"},
           {"max_tokens", 0},
           {"invented", true}}}},
        {{"id", "post"},
         {"node_type", "StructuredJsonParseNode"},
         {"depends_on", {"llm"}}}}}};
  const auto report =
      ValidateWithRemediation(pipeline, EntityExtractTestBoundary());
  EXPECT_FALSE(report.ok);
  std::set<DiagnosticCode> codes;
  for (const auto& diagnostic : report.diagnostics)
    codes.insert(diagnostic.code);
  EXPECT_TRUE(codes.count(DiagnosticCode::kUnknownConfigField));
  EXPECT_TRUE(codes.count(DiagnosticCode::kConfigFieldRange));
  EXPECT_TRUE(codes.count(DiagnosticCode::kModelCapabilityMismatch));

  // 验证外部 JSON 序列化一致性
  auto json_rep = report.ToJson();
  std::set<std::string> json_codes;
  for (const auto& item : json_rep["diagnostics"]) {
    json_codes.insert(item["code"].get<std::string>());
  }
  EXPECT_TRUE(json_codes.count("UNKNOWN_CONFIG_FIELD"));
  EXPECT_TRUE(json_codes.count("CONFIG_FIELD_RANGE"));
  EXPECT_TRUE(json_codes.count("MODEL_CAPABILITY_MISMATCH"));
}

TEST(PipelineValidatorTest, SerializedModelBranchesRunInSeparateLayers) {
  const nlohmann::json config = {
      {"max_parallel_workers", 4},
      {"models",
       {{{"model_id", "serialized_llm"},
         {"model_type", "test_biz_llm"},
         {"backend", "test_causal_lm_backend"},
         {"model_path", "fixture.gguf"}}}},
      {"pipeline",
       {{{"id", "pre"},
         {"node_type", "TextTemplateNode"},
         {"depends_on", nlohmann::json::array()},
         {"inputs", {{"primary", "input_sentences"}}},
         {"outputs", {{"text", "prompt_text"}}}},
        {{"id", "left"},
         {"node_type", "LlmGenerateNode"},
         {"depends_on", {"pre"}},
         {"config", {{"bind_model", "serialized_llm"}}},
         {"inputs", {{"prompt", "prompt_text"}}},
         {"outputs", {{"text", "ans_left"}}}},
        {{"id", "right"},
         {"node_type", "LlmGenerateNode"},
         {"depends_on", {"pre"}},
         {"config", {{"bind_model", "serialized_llm"}}},
         {"inputs", {{"prompt", "prompt_text"}}},
         {"outputs", {{"text", "ans_right"}}}},
        {{"id", "post"},
         {"node_type", "StructuredJsonParseNode"},
         {"depends_on", {"left", "right"}},
         {"inputs", {{"text", "ans_left"}}},
         {"outputs", {{"document", "extracted_entities"}}}}}}};

  const auto report =
      ValidateWithRemediation(config, EntityExtractTestBoundary());
  ASSERT_TRUE(report.ok) << report.ToJson().dump(2);
  EXPECT_EQ(report.topological_layers,
            (std::vector<std::vector<std::string>>{
                {"pre"}, {"left"}, {"right"}, {"post"}}));
  EXPECT_EQ(report.topological_order,
            (std::vector<std::string>{"pre", "left", "right", "post"}));
}

TEST(PipelineValidatorTest, TableDrivenParityMatrix) {
  std::ifstream stream(
      "tests/fixtures/pipelines/validation/invalid_pipeline_cases.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json fixtures;
  stream >> fixtures;

  for (const auto& test : fixtures["cases"]) {
    SCOPED_TRACE(test["name"].get<std::string>());
    const auto& config = test["pipeline"];

    // 0. 接入层先拆出 io 并由所选 converter 组成边界。文档在接入层或 Core 的
    // 结构检查中就失败时，Core 直接检查去掉 io 的文档。
    PreparedDeployment prepared;
    DeploymentDiagnostic prepare_diagnostic;
    nlohmann::json core_config = config;
    PipelineIoBoundary boundary;
    if (PrepareDeploymentDocument(config, {}, &prepared, &prepare_diagnostic)) {
      core_config = prepared.neutral_pipeline_json;
      boundary = prepared.io_boundary;
    } else {
      core_config.erase("io");
    }
    const std::string core_path = test["primary_path"].get<std::string>();
    const bool unknown_root_field = test["primary_code"] == "UNKNOWN_FIELD" &&
                                    core_path.find('/', 1) == std::string::npos;

    // 1. Validator 是完整结构化报告的基准。
    auto plan = PipelineValidator::ValidateAndPlan(core_config, boundary);
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

    // 2. Pipeline 直接映射 Validator 的第一条诊断，不重新计算。
    Pipeline pipeline;
    PipelineDiagnostic pipe_diag;
    bool built = BuildTestPipeline(pipeline, core_config, boundary, &pipe_diag);
    EXPECT_FALSE(built);
    EXPECT_EQ(pipeline.GetState(), Pipeline::State::kFailed);
    EXPECT_EQ(DiagnosticCodeName(pipe_diag.code),
              test["primary_code"].get<std::string>());
    EXPECT_EQ(pipe_diag.path, test["primary_path"].get<std::string>());
    EXPECT_EQ(pipe_diag.message, primary["message"].get<std::string>());

    // 3. 共享运行时必须在实例化前失败，并在其内部 C++ 错误边界中
    // 保留主结构化诊断。
    std::unique_ptr<ValidatedIoPlan> io_plan;
    std::string resolve_error;
    DeploymentDiagnostic resolve_diagnostic;
    int resolve_result = IoPlanResolver::ResolveFromPipelineJson(
        config, "./models", &io_plan, &resolve_error, &resolve_diagnostic);
    EXPECT_NE(resolve_result, 0);
    EXPECT_EQ(io_plan, nullptr);
    EXPECT_EQ(resolve_diagnostic.code,
              unknown_root_field ? "DEPLOYMENT_ERROR"
                                 : test["primary_code"].get<std::string>());
    EXPECT_EQ(resolve_diagnostic.path, core_path);
    EXPECT_NE(resolve_error.find(resolve_diagnostic.path), std::string::npos);
  }
}

TEST(PipelineValidatorTest, WhisperPipelineValidationDependsOnBackend) {
  std::ifstream stream("configs/pipeline_audio_asr_intent_cpu.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json pipeline;
  stream >> pipeline;
  PipelineIoBoundary boundary;
  pipeline = PrepareExternalFixtureForCore(pipeline, &boundary);
  const auto report = ValidateWithRemediation(pipeline, boundary);
#ifdef HAVE_WHISPERCPP
  EXPECT_TRUE(report.ok) << report.ToJson().dump(2);
  const auto plan = PipelineValidator::ValidateAndPlan(pipeline, boundary);
  EXPECT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
#else
  EXPECT_FALSE(report.ok);
  EXPECT_TRUE(std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
                          [](const ValidationDiagnostic& diagnostic) {
                            return diagnostic.code ==
                                       DiagnosticCode::kUnknownBackend &&
                                   diagnostic.path == "/models/0/backend";
                          }));
#endif
}

}  // namespace
}  // namespace llm_edgeflow

namespace llm_edgeflow {
TEST(PipelineValidatorTest,
     NestedAndCrossFieldErrorsFailBeforeMaterialization) {
  const std::vector<std::pair<std::string, nlohmann::json>> cases = {
      {"TextChunkNode", {{"chunk_size", 8}, {"overlap", 8}}},
      {"TextTemplateNode", {{"template", "{{unknown}}"}}},
      {"TextTemplateNode", {{"template", "{{unclosed"}}},
      {"StructuredJsonParseNode", {{"field_types", {{"x", "unsupported"}}}}},
      {"StructuredJsonParseNode",
       {{"required_fields", {"risk"}}, {"fallback", nlohmann::json::object()}}},
      {"TextCorpusSourceNode", {{"corpus", {"valid", 42}}}},
      {"TextRuleMatchNode",
       {{"rules", {{{"strategy", "regex"}, {"pattern", "["}}}}}},
  };
  for (const auto& [type, config] : cases) {
    SCOPED_TRACE(type + config.dump());
    std::ifstream stream("configs/pipeline_keyword_match_rules.json");
    nlohmann::json root;
    stream >> root;
    PipelineIoBoundary boundary;
    root = PrepareExternalFixtureForCore(root, &boundary);
    root["pipeline"].push_back({{"id", "invalid"},
                                {"node_type", type},
                                {"depends_on", nlohmann::json::array()},
                                {"config", config}});
    const auto report = ValidateWithRemediation(root, boundary);
    EXPECT_FALSE(report.ok);
    EXPECT_TRUE(
        std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
                    [](const auto& d) {
                      return d.node_id == "invalid" &&
                             (d.code == DiagnosticCode::kInvalidCombination ||
                              d.code == DiagnosticCode::kConfigFieldType ||
                              d.code == DiagnosticCode::kConfigFieldEnum);
                    }))
        << report.ToJson();
    Pipeline pipeline;
    EXPECT_FALSE(BuildTestPipeline(pipeline, root, boundary));
    auto node = NodeRegistry::Instance().Create(type);
    SessionContext session;
    EXPECT_FALSE(InitNodeForTest(*node, config, &session));
  }
}

TEST(PipelineValidatorTest, UnconnectedOptionalPortStaysAbsentAtRuntime) {
  std::ifstream stream("configs/pipeline_keyword_match_rules.json");
  nlohmann::json root;
  stream >> root;
  PipelineIoBoundary boundary;
  root = PrepareExternalFixtureForCore(root, &boundary);
  root["pipeline"] =
      nlohmann::json::array({{{"id", "a"},
                              {"node_type", "TextTemplateNode"},
                              {"depends_on", nlohmann::json::array()},
                              {"config", {{"template", "UNDECLARED"}}},
                              {"inputs", {{"primary", "input_sentences"}}},
                              {"outputs", {{"text", "context"}}}},
                             {{"id", "b"},
                              {"node_type", "TextTemplateNode"},
                              {"depends_on", {"a"}},
                              {"config", {{"template", "{{primary}}"}}},
                              {"inputs", {{"primary", "input_sentences"}}},
                              {"outputs", {{"text", "rendered"}}}},
                             {{"id", "rule"},
                              {"node_type", "TextRuleMatchNode"},
                              {"depends_on", {"b"}},
                              {"inputs", {{"text", "rendered"}}},
                              {"outputs", {{"matches", "rule_matches"}}}}});
  const auto plan = PipelineValidator::ValidateAndPlan(root, boundary);
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson();
  EXPECT_EQ(plan.node_plans.at("b").FindPort("context"), nullptr);
  Pipeline pipeline;
  ASSERT_TRUE(BuildTestPipeline(pipeline, root, boundary));
  AlgContext ctx;
  ctx.Publish("input_sentences", TextBatch{{0, 0, "USER"}});
  ASSERT_EQ(pipeline.Execute(&ctx), 0);
  ASSERT_NE(ctx.Read<TextBatch>("rendered"), nullptr);
  EXPECT_EQ(ctx.Read<TextBatch>("rendered")->at(0).data, "USER");
}

TEST(PipelineValidatorTest,
     ExplainAcceptsDataConnectionWithoutExplicitDependency) {
  std::ifstream stream(
      "demo/fixtures/mock/pipeline_entity_extract_custom.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json root;
  stream >> root;
  PipelineIoBoundary boundary;
  root = PrepareExternalFixtureForCore(root, &boundary);
  for (auto& node : root["pipeline"]) node.erase("depends_on");

  const auto report = ExplainPipeline(root, boundary);
  EXPECT_TRUE(report.ok) << report.ToJson().dump(2);
  EXPECT_TRUE(report.diagnostics.empty());
  const auto plan = PipelineValidator::ValidateAndPlan(root, boundary);
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
  ASSERT_EQ(plan.report.topological_order.size(), 2U);
  EXPECT_EQ(plan.report.topological_order.front(), "custom_prompt");
}

TEST(PipelineValidatorTest, ExplainReturnsCandidateFixForUnknownConfigField) {
  std::ifstream stream(
      "demo/fixtures/mock/pipeline_entity_extract_custom.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json root;
  stream >> root;
  PipelineIoBoundary boundary;
  root = PrepareExternalFixtureForCore(root, &boundary);

  // 把 "temperature" 误拼为 "temprature"
  root["pipeline"][0]["config"]["temprature"] = 0.1;
  root["pipeline"][0]["config"].erase("temperature");

  const auto report = ExplainPipeline(root, boundary);
  EXPECT_FALSE(report.ok);
  ASSERT_FALSE(report.diagnostics.empty());

  const ValidationDiagnostic* target_diag = nullptr;
  for (const auto& diag : report.diagnostics) {
    if (diag.code == DiagnosticCode::kUnknownConfigField &&
        diag.path == "/pipeline/0/config/temprature") {
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
        op.value("from", "") == "/pipeline/0/config/temprature" &&
        op.value("path", "") == "/pipeline/0/config/temperature") {
      found_move = true;
      break;
    }
  }
  EXPECT_TRUE(found_move);

  // 应用补丁后恢复为完全合法的 Pipeline
  const auto patched = root.patch(fix.patch);
  const auto verified_report = ValidateWithRemediation(patched, boundary);
  EXPECT_TRUE(verified_report.ok) << verified_report.ToJson().dump(2);
}

TEST(PipelineValidatorTest, ExplainRespectsFixBounds) {
  // 构造含 5 个独立上游 Node 和 4 个依赖非法的消费 Node 的 Pipeline。
  // 若不设上限，每个消费者会产生 8 个候选修复，共 32 个。
  // Explain 必须限制每条诊断最多 3 个修复、每份报告最多 8 个修复。
  nlohmann::json root = {{"models", nlohmann::json::array()},
                         {"pipeline", nlohmann::json::array()}};

  for (int i = 0; i < 5; ++i) {
    root["pipeline"].push_back({{"id", "upstream_" + std::to_string(i)},
                                {"node_type", "TextCorpusSourceNode"},
                                {"depends_on", nlohmann::json::array()},
                                {"config", {{"corpus", {"sample"}}}}});
  }

  for (int i = 0; i < 4; ++i) {
    root["pipeline"].push_back({{"id", "consumer_" + std::to_string(i)},
                                {"node_type", "TextCorpusSourceNode"},
                                {"depends_on", {"nonexistent_node"}},
                                {"config", {{"corpus", {"sample"}}}}});
  }

  const auto report = ExplainPipeline(root, KeywordMatchTestBoundary());
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
  PipelineIoBoundary boundary;
  root = PrepareExternalFixtureForCore(root, &boundary);

  const auto report = ExplainPipeline(root, boundary);
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
  PipelineIoBoundary boundary;
  root = PrepareExternalFixtureForCore(root, &boundary);

  // 引入两个相互独立的错误：
  // 1. custom_prompt 中的配置字段拼写错误 ("temprature" 而非 "temperature")
  root["pipeline"][0]["config"]["temprature"] = 0.1;
  root["pipeline"][0]["config"].erase("temperature");

  // 2. node_2_StructuredJsonParseNode 中仍未解决的非法配置
  root["pipeline"][1]["config"]["field_types"] = {
      {"field_x", "unsupported_type"}};

  const auto report = ExplainPipeline(root, boundary);
  EXPECT_FALSE(report.ok);
  ASSERT_GE(report.diagnostics.size(), 2U);

  const ValidationDiagnostic* typo_diag = nullptr;
  for (const auto& diag : report.diagnostics) {
    if (diag.code == DiagnosticCode::kUnknownConfigField &&
        diag.path == "/pipeline/0/config/temprature") {
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
  PipelineIoBoundary boundary;
  root = PrepareExternalFixtureForCore(root, &boundary);

  // 显式把输入连接到没有生产者的键。
  root["pipeline"][1]["inputs"]["text"] = "missing_result";

  const auto report = ValidateWithRemediation(root, boundary);
  EXPECT_FALSE(report.ok);
  ASSERT_FALSE(report.diagnostics.empty());

  const ValidationDiagnostic* target_diag = nullptr;
  for (const auto& diag : report.diagnostics) {
    if (diag.code == DiagnosticCode::kMissingInputProducer &&
        diag.node_id == "node_2_StructuredJsonParseNode" &&
        diag.port == "text") {
      target_diag = &diag;
      break;
    }
  }
  ASSERT_NE(target_diag, nullptr);
  ASSERT_TRUE(target_diag->remediation.has_value());
  EXPECT_NE(RemediationCauseName(target_diag->remediation->cause), "UNKNOWN");
  EXPECT_FALSE(target_diag->remediation->summary.empty());
  EXPECT_FALSE(target_diag->remediation->facts.empty());
  EXPECT_TRUE(target_diag->remediation->fixes.empty());
}

TEST(PipelineValidatorTest, ExplainCapsVerificationAttemptsAtEight) {
  nlohmann::json root = {{"models", nlohmann::json::array()},
                         {"pipeline",
                          {{{"id", "custom_prompt"},
                            {"node_type", "PromptGuidedLlmNode"},
                            {"depends_on", nlohmann::json::array()},
                            {"inputs", {{"input", "input_sentences"}}},
                            {"outputs", {{"output", "llm_raw_answer"}}},
                            {"config",
                             {{"bind_model", "missing_model"},
                              {"prompt_template", "Hello {{input}}"},
                              {"strip_markdown", true},
                              {"temperature", 0.1},
                              {"max_tokens", 64}}}},
                           {{"id", "node_2_StructuredJsonParseNode"},
                            {"node_type", "StructuredJsonParseNode"},
                            {"depends_on", {"custom_prompt"}},
                            {"inputs", {{"text", "llm_raw_answer"}}},
                            {"outputs", {{"document", "extracted_entities"}}},
                            {"config", {{"failure_policy", "fail"}}}}}}};

  for (int i = 0; i < 10; ++i) {
    char name_buf[32];
    std::snprintf(name_buf, sizeof(name_buf), "a_invalid_%02d", i);
    root["models"].push_back({
        {"model_id", name_buf},
        {"model_type", "test_biz_llm"},
        {"backend", "unknown_backend"},
        {"model_path", "demo/fixtures/mock/artifacts/neutral-llm.fixture"},
        {"model_config", {{"max_batch_size", 2}, {"max_seq_len", 512}}},
        {"backend_config", nlohmann::json::object()},
    });
  }

  root["models"].push_back({
      {"model_id", "z_valid"},
      {"model_type", "test_biz_llm"},
      {"backend", "test_causal_lm_backend"},
      {"model_path", "demo/fixtures/mock/artifacts/neutral-llm.fixture"},
      {"model_config", {{"max_batch_size", 2}, {"max_seq_len", 512}}},
      {"backend_config", nlohmann::json::object()},
  });

  const auto report = ExplainPipeline(root, EntityExtractTestBoundary());
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
}

TEST(PipelineValidatorTest, ExplainRejectsInvalidModelCandidates) {
  nlohmann::json root = {{"models",
                          {{{"model_id", "broken_model"},
                            {"model_type", "bge_embedding"},
                            {"backend", "test_causal_lm_backend"},
                            {"model_path", "fixture.bin"},
                            {"model_config", {{"max_batch_size", -1}}},
                            {"backend_config", nlohmann::json::object()}}}},
                         {"pipeline",
                          {{{"id", "llm_node"},
                            {"node_type", "PromptGuidedLlmNode"},
                            {"depends_on", nlohmann::json::array()},
                            {"inputs", {{"input", "input_sentences"}}},
                            {"outputs", {{"output", "llm_raw_answer"}}},
                            {"config",
                             {{"bind_model", "missing_model"},
                              {"prompt_template", "Hello {{input}}"},
                              {"strip_markdown", true},
                              {"temperature", 0.1},
                              {"max_tokens", 64}}}}}}};

  const auto report = ExplainPipeline(root, EntityExtractTestBoundary());
  EXPECT_FALSE(report.ok);

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

TEST(PipelineValidatorTest, SplitItemsFeedItemWiseNodeButNotPerRequestEgress) {
  nlohmann::json root = {
      {"pipeline",
       {{{"id", "chunk"},
         {"node_type", "TextChunkNode"},
         {"config", {{"chunk_size", 4}}},
         {"inputs", {{"text", "input_sentences"}}},
         {"outputs",
          {{"chunks", "sentence_chunks"},
           {"chunk_counts", "sentence_chunk_counts"}}}},
        {{"id", "rules_per_chunk"},
         {"node_type", "TextRuleMatchNode"},
         {"config", {{"categories", {{"SYSTEM_INIT", {"初始化"}}}}}},
         {"inputs", {{"text", "sentence_chunks"}}},
         {"outputs", {{"matches", "chunk_matches"}}}},
        {{"id", "rules_per_request"},
         {"node_type", "TextRuleMatchNode"},
         {"config", {{"categories", {{"SYSTEM_INIT", {"初始化"}}}}}},
         {"inputs", {{"text", "input_sentences"}}},
         {"outputs", {{"matches", "rule_matches"}}}}}}};

  const auto accepted =
      ValidateWithRemediation(root, KeywordMatchTestBoundary());
  EXPECT_TRUE(accepted.ok) << accepted.ToJson().dump();

  // 逐分块的匹配结果不能作为每请求一行的输出项数据。
  root["pipeline"][1]["outputs"]["matches"] = "rule_matches";
  root["pipeline"][2]["outputs"]["matches"] = "request_matches";
  const auto rejected =
      ValidateWithRemediation(root, KeywordMatchTestBoundary());
  ASSERT_FALSE(rejected.ok);
  ASSERT_EQ(rejected.diagnostics.size(), 1u) << rejected.ToJson().dump();
  EXPECT_EQ(rejected.diagnostics[0].code,
            DiagnosticCode::kPortCardinalityMismatch);
  EXPECT_EQ(rejected.diagnostics[0].path, "/io/output");
  EXPECT_EQ(rejected.diagnostics[0].port, "rule_matches");
  const auto& egress = rejected.diagnostics[0];
  ASSERT_TRUE(egress.remediation.has_value());
  EXPECT_EQ(egress.remediation->cause, RemediationCause::kPortFlowMismatch);
  EXPECT_NE(egress.remediation->summary.find("生产者 'rules_per_chunk'"),
            std::string::npos);
  EXPECT_NE(egress.remediation->summary.find("消费者 '$io_output'"),
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
  EXPECT_EQ(facts.at("producer_id"), "rules_per_chunk");
  EXPECT_EQ(facts.at("consumer_id"), "$io_output");
  EXPECT_EQ(facts.at("bound_key"), "rule_matches");
  EXPECT_EQ(facts.at("actual"), declaration);
  EXPECT_EQ(facts.at("expected"), declaration);
  EXPECT_EQ(facts.at("actual_shape"), actual_shape);
  EXPECT_EQ(facts.at("expected_shape"), expected_shape);
  EXPECT_EQ(egress.facts, facts);
  EXPECT_EQ(egress.ToJson().at("facts"), facts);
}

TEST(PipelineValidatorTest, ExplainReturnsPortFlowMismatchRemediation) {
  nlohmann::json root = {{"models", nlohmann::json::array()},
                         {"pipeline",
                          {{{"id", "producer"},
                            {"node_type", "TextChunkNode"},
                            {"depends_on", nlohmann::json::array()},
                            {"inputs", {{"text", "input_sentences"}}},
                            {"outputs", {{"chunks", "chunk_text"}}}},
                           {{"id", "consumer"},
                            {"node_type", "TextEmbeddingNode"},
                            {"depends_on", {"producer"}},
                            {"inputs", {{"text", "chunk_text"}}},
                            {"outputs", {{"embedding", "chunk_embeddings"}}},
                            {"config", {{"lifetime", "session"}}}}}}};

  const auto report = ValidateWithRemediation(root, KeywordMatchTestBoundary());
  EXPECT_FALSE(report.ok);
  ASSERT_FALSE(report.diagnostics.empty());

  // 请求作用域的分块不能支撑会话作用域的 embedding 缓存。
  const ValidationDiagnostic* target_diag = nullptr;
  for (const auto& diag : report.diagnostics) {
    if (diag.code == DiagnosticCode::kPortLifetimeMismatch &&
        diag.node_id == "consumer" && diag.port == "text") {
      target_diag = &diag;
      break;
    }
  }

  ASSERT_NE(target_diag, nullptr);
  ASSERT_TRUE(target_diag->remediation.has_value());
  EXPECT_EQ(target_diag->remediation->cause,
            RemediationCause::kPortFlowMismatch);
  EXPECT_EQ(target_diag->remediation->facts.value("bound_key", ""),
            "chunk_text");
  EXPECT_EQ(target_diag->remediation->facts.value("producer_id", ""),
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
  EXPECT_EQ(target_diag->remediation->facts.at("consumer_id"), "consumer");
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
  nlohmann::json root = {
      {"models", nlohmann::json::array()},
      {"pipeline",
       {{{"id", "dep_a"},
         {"node_type", "TextCorpusSourceNode"},
         {"depends_on", nlohmann::json::array()},
         {"config", {{"corpus", {"a"}}}}},
        {{"id", "dep_b"},
         {"node_type", "TextCorpusSourceNode"},
         {"depends_on", nlohmann::json::array()},
         {"config", {{"corpus", {"b"}}}}},
        {{"id", "consumer"},
         {"node_type", "TextCorpusSourceNode"},
         {"depends_on", {"dep_a", "dep_a", "dep_b", "dep_b"}},
         {"config", {{"corpus", {"c"}}}}}}}};

  const auto report = ExplainPipeline(root, KeywordMatchTestBoundary());
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
  EXPECT_EQ(diag_a->remediation->facts.value("dependency_id", ""), "dep_a");
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
  EXPECT_EQ(diag_b->remediation->facts.value("dependency_id", ""), "dep_b");
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

TEST(PipelineValidatorTest, BlackboardKeyAndBoundInputContracts) {
  constexpr auto key = MakeBlackboardKey<TextBatch>("custom_key");
  EXPECT_STREQ(key.name, "custom_key");
  EXPECT_STREQ(key.type_id, "TextBatch");

  const BlackboardKey<TextBatch> mismatched_key{"key", "WrongType"};
  EXPECT_THROW((BoundInput<TextBatch>(mismatched_key)), std::invalid_argument);
  EXPECT_THROW((BoundOutput<TextBatch>(mismatched_key)), std::invalid_argument);

  EXPECT_NO_THROW((BoundInput<TextBatch>(key)));
  EXPECT_NO_THROW((BoundOutput<TextBatch>(key)));
}
}  // namespace llm_edgeflow

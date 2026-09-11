#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "adapter/shared_algorithm_runtime.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"
#include "nodes/node_base.h"

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
  definition.biz_names = {"keyword_match_v1"};
  return definition;
}

REGISTER_NODE_WITH_DEFINITION(StudioCatalogProbeNode,
                              StudioCatalogProbeDefinition());

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
  const auto filtered = PipelineCatalog::ToJson("keyword_match_v1");
  EXPECT_TRUE(std::any_of(
      filtered["nodes"].begin(), filtered["nodes"].end(), [](const auto& item) {
        return item["node_type"] == "StudioCatalogProbeNode";
      }));
}

TEST(BlackboardKeyTest, TypedOverloadsShareTheRuntimeKey) {
  constexpr BlackboardKey<std::vector<std::string>> key{
      "studio_values", "std::vector<std::string>"};
  AlgContext context;
  ASSERT_TRUE(context.Publish(key, std::vector<std::string>{"a", "b"}));
  ASSERT_TRUE(context.Has(key));
  ASSERT_NE(context.Read(key), nullptr);
  EXPECT_EQ(*context.Read(key), (std::vector<std::string>{"a", "b"}));
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
    const auto report = PipelineValidator::Validate(pipeline);
    EXPECT_TRUE(report.ok) << entry.path() << "\n" << report.ToJson().dump(2);
    ++validated;
  }
  EXPECT_GT(validated, 0U);
  EXPECT_EQ(validated + skipped_optional, candidates);
}

TEST(PipelineValidatorTest, RejectsRemovedRuleCategoriesField) {
  std::ifstream stream("configs/pipeline_keyword_match_rules.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json pipeline;
  stream >> pipeline;
  ASSERT_FALSE(pipeline["pipeline"].empty());
  auto& config = pipeline["pipeline"][0]["config"];
  config["default_categories"] = config["categories"];
  config.erase("categories");

  const auto report = PipelineValidator::Validate(pipeline);
  ASSERT_FALSE(report.ok);
  EXPECT_TRUE(std::any_of(
      report.diagnostics.begin(), report.diagnostics.end(),
      [](const ValidationDiagnostic& diagnostic) {
        return diagnostic.code == DiagnosticCode::kUnknownConfigField &&
               diagnostic.path == "/pipeline/0/config/default_categories";
      }));
}

TEST(PipelineValidatorTest, ModelPathsUseLexicalChecksWithoutDeploymentRoots) {
  std::ifstream stream("demo/fixtures/mock/pipeline_doc_qa.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json pipeline;
  ASSERT_NO_THROW(stream >> pipeline);

  for (const std::string& safe_path :
       {std::string("missing/artifact.bin"), std::string("..name/artifact.bin"),
        std::string("missing/../artifact.bin"),
        std::filesystem::absolute("missing/artifact.bin").string()}) {
    pipeline["models"][0]["model_path"] = safe_path;
    const auto report = PipelineValidator::Validate(pipeline);
    EXPECT_TRUE(report.ok) << safe_path << "\n" << report.ToJson().dump(2);
  }

  for (const char* unsafe_path :
       {"..", "../artifact.bin", "missing/../../artifact.bin",
        "..\\artifact.bin"}) {
    pipeline["models"][0]["model_path"] = unsafe_path;
    const auto report = PipelineValidator::Validate(pipeline);
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

TEST(PipelineValidatorTest, ReportsCycle) {
  const nlohmann::json pipeline = {{"biz_name", "keyword_match_v1"},
                                   {"pipeline",
                                    {{{"id", "a"},
                                      {"node_type", "TextRuleMatchNode"},
                                      {"depends_on", {"b"}}},
                                     {{"id", "b"},
                                      {"node_type", "TextRuleMatchNode"},
                                      {"depends_on", {"a"}}}}}};
  const auto report = PipelineValidator::Validate(pipeline);
  EXPECT_FALSE(report.ok);
  std::set<DiagnosticCode> codes;
  for (const auto& diagnostic : report.diagnostics)
    codes.insert(diagnostic.code);
  EXPECT_TRUE(codes.count(DiagnosticCode::kDagCycle));
}

TEST(PipelineValidatorTest, ReportsDuplicateEdge) {
  const nlohmann::json pipeline = {{"biz_name", "keyword_match_v1"},
                                   {"pipeline",
                                    {{{"id", "a"},
                                      {"node_type", "TextRuleMatchNode"},
                                      {"depends_on", nlohmann::json::array()}},
                                     {{"id", "b"},
                                      {"node_type", "TextRuleMatchNode"},
                                      {"depends_on", {"a", "a"}}}}}};
  const auto report = PipelineValidator::Validate(pipeline);
  ASSERT_FALSE(report.ok);
  ASSERT_FALSE(report.diagnostics.empty());
  EXPECT_EQ(report.diagnostics.front().code,
            DiagnosticCode::kDuplicateDependency);
  EXPECT_EQ(report.diagnostics.front().path, "/pipeline/1/depends_on/1");
}

TEST(PipelineValidatorTest, ReportsConfigAndCapabilityErrors) {
  const nlohmann::json pipeline = {
      {"biz_name", "entity_extract_v1"},
      {"models",
       {{{"model_id", "llm_model_v1"},
         {"capability", "embedding"},
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
         {"config", {{"max_tokens", 0}, {"invented", true}}}},
        {{"id", "post"},
         {"node_type", "StructuredJsonParseNode"},
         {"depends_on", {"llm"}}}}}};
  const auto report = PipelineValidator::Validate(pipeline);
  EXPECT_FALSE(report.ok);
  std::set<DiagnosticCode> codes;
  for (const auto& diagnostic : report.diagnostics)
    codes.insert(diagnostic.code);
  EXPECT_TRUE(codes.count(DiagnosticCode::kUnknownConfigField));
  EXPECT_TRUE(codes.count(DiagnosticCode::kConfigFieldRange));
  EXPECT_TRUE(codes.count(DiagnosticCode::kModelCapabilityMismatch));

  // Verify external JSON serialization parity
  auto json_rep = report.ToJson();
  std::set<std::string> json_codes;
  for (const auto& item : json_rep["diagnostics"]) {
    json_codes.insert(item["code"].get<std::string>());
  }
  EXPECT_TRUE(json_codes.count("UNKNOWN_CONFIG_FIELD"));
  EXPECT_TRUE(json_codes.count("CONFIG_FIELD_RANGE"));
  EXPECT_TRUE(json_codes.count("MODEL_CAPABILITY_MISMATCH"));
}

TEST(PipelineValidatorTest, TableDrivenParityMatrix) {
  std::ifstream stream(
      "tests/fixtures/pipelines/validation/invalid_pipeline_cases.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json fixtures;
  stream >> fixtures;
  ASSERT_EQ(fixtures["schema_version"], 1);

  for (const auto& test : fixtures["cases"]) {
    SCOPED_TRACE(test["name"].get<std::string>());
    const auto& config = test["pipeline"];

    // 1. Validator is the complete structured-report baseline.
    auto plan = PipelineValidator::ValidateAndPlan(config);
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

    // 2. Pipeline maps the first Validator diagnostic without recomputing it.
    Pipeline pipeline;
    PipelineDiagnostic pipe_diag;
    bool built = pipeline.BuildFromJson(config, &pipe_diag);
    EXPECT_FALSE(built);
    EXPECT_EQ(pipeline.GetState(), Pipeline::State::kFailed);
    EXPECT_EQ(static_cast<int>(pipe_diag.code),
              test["pipeline_error_code"].get<int>());
    EXPECT_EQ(pipe_diag.path, test["primary_path"].get<std::string>());
    EXPECT_NE(pipe_diag.message.find(test["primary_code"].get<std::string>()),
              std::string::npos);

    // 3. The shared runtime must fail before materialization and preserve the
    // primary structured diagnostic in its internal C++ error boundary.
    std::unique_ptr<SharedAlgorithmRuntime> runtime;
    std::string runtime_error;
    int runtime_result = SharedAlgorithmRuntime::CreateFromPipelineJson(
        config, 0, "./models",
        static_cast<CompanyAlgBizType>(test["biz_type"].get<int>()), &runtime,
        &runtime_error);
    EXPECT_EQ(runtime_result, test["runtime_error_code"].get<int>());
    EXPECT_EQ(runtime, nullptr);
    EXPECT_NE(runtime_error.find(test["primary_code"].get<std::string>()),
              std::string::npos);
    EXPECT_NE(runtime_error.find(test["primary_path"].get<std::string>()),
              std::string::npos);
  }
}

TEST(PipelineValidatorTest, WhisperPipelineValidationDependsOnBackend) {
  std::ifstream stream("configs/pipeline_audio_asr_cpu.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json pipeline;
  stream >> pipeline;
  const auto report = PipelineValidator::Validate(pipeline);
#ifdef HAVE_WHISPERCPP
  EXPECT_TRUE(report.ok) << report.ToJson().dump(2);
  const auto plan = PipelineValidator::ValidateAndPlan(pipeline);
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
      {"TextTemplateNode", {{"values", {{"x", 42}}}}},
      {"TextTemplateNode", {{"template", "{{unclosed"}}},
      {"StructuredJsonParseNode", {{"field_types", {{"x", "unsupported"}}}}},
      {"StructuredJsonParseNode",
       {{"required_fields", {"risk"}}, {"fallback_json", "{}"}}},
      {"TextCorpusSourceNode", {{"corpus", {"valid", 42}}}},
      {"TextRuleMatchNode",
       {{"rules", {{{"strategy", "regex"}, {"pattern", "["}}}}}},
  };
  for (const auto& [type, config] : cases) {
    SCOPED_TRACE(type + config.dump());
    std::ifstream stream("configs/pipeline_keyword_match_rules.json");
    nlohmann::json root;
    stream >> root;
    root["pipeline"].push_back({{"id", "invalid"},
                                {"node_type", type},
                                {"depends_on", nlohmann::json::array()},
                                {"config", config}});
    const auto report = PipelineValidator::Validate(root);
    EXPECT_FALSE(report.ok);
    EXPECT_TRUE(
        std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
                    [](const auto& d) {
                      return d.node_id == "invalid" &&
                             (d.code == DiagnosticCode::kInvalidCombination ||
                              d.code == DiagnosticCode::kConfigFieldType);
                    }))
        << report.ToJson();
    Pipeline pipeline;
    EXPECT_FALSE(pipeline.BuildFromJson(root));
    auto node = NodeRegistry::Instance().Create(type);
    SessionContext session;
    NodeInitContext init;
    init.config = &config;
    init.session_ctx = &session;
    EXPECT_FALSE(node->Init(init));
  }
}

TEST(PipelineValidatorTest, UnconnectedOptionalPortStaysAbsentAtRuntime) {
  std::ifstream stream("configs/pipeline_keyword_match_rules.json");
  nlohmann::json root;
  stream >> root;
  root["pipeline"] = nlohmann::json::array(
      {{{"id", "a"},
        {"node_type", "TextTemplateNode"},
        {"depends_on", nlohmann::json::array()},
        {"config", {{"template", "UNDECLARED"}}},
        {"ports",
         {{"inputs", {{"primary", "input_sentences"}}},
          {"outputs", {{"text", "context_text"}}}}}},
       {{"id", "b"},
        {"node_type", "TextTemplateNode"},
        {"depends_on", nlohmann::json::array()},
        {"config", {{"template", "{{primary}}|{{context}}"}}},
        {"ports",
         {{"inputs", {{"primary", "input_sentences"}}},
          {"outputs", {{"text", "rendered"}}}}}},
       {{"id", "rule"},
        {"node_type", "TextRuleMatchNode"},
        {"depends_on", {"b"}},
        {"ports",
         {{"inputs", {{"text", "rendered"}}},
          {"outputs", {{"matches", "rule_matches"}}}}}}});
  root["pipeline"][1]["config"]["missing_variable_policy"] = "empty";
  const auto plan = PipelineValidator::ValidateAndPlan(root);
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson();
  EXPECT_EQ(plan.node_plans.at("b").FindPort("context_text"), nullptr);
  Pipeline pipeline;
  ASSERT_TRUE(pipeline.BuildFromJson(root));
  AlgContext ctx;
  ctx.Publish("input_sentences", TextBatch{{0, 0, "USER"}});
  ASSERT_EQ(pipeline.Execute(&ctx), 0);
  ASSERT_NE(ctx.Read<TextBatch>("rendered"), nullptr);
  EXPECT_EQ(ctx.Read<TextBatch>("rendered")->at(0).data, "USER|");
}

TEST(PipelineValidatorTest,
     ExplainReturnsCandidateFixForProducerNotDependencyAncestor) {
  std::ifstream stream(
      "demo/fixtures/mock/pipeline_entity_extract_custom.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json root;
  stream >> root;

  // Node 0: custom_prompt (produces "llm_raw_answer")
  // Node 1: node_2_StructuredJsonParseNode (consumes "llm_raw_answer" on port
  // "text") Clear depends_on so node 1 does not depend on custom_prompt:
  root["pipeline"][1]["depends_on"] = nlohmann::json::array();

  const auto report = PipelineValidator::Explain(root);
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
  const auto& rem = *target_diag->remediation;
  EXPECT_EQ(rem.schema_version, 1);
  EXPECT_EQ(rem.cause, "producer_not_dependency_ancestor");
  EXPECT_EQ(rem.facts.value("producer_id", ""), "custom_prompt");
  EXPECT_EQ(rem.facts.value("bound_key", ""), "llm_raw_answer");

  ASSERT_FALSE(rem.fixes.empty());
  const auto& fix = rem.fixes.front();
  EXPECT_EQ(fix.verification, "pipeline_valid");
  EXPECT_FALSE(fix.patch.empty());

  // Verify the RFC 6902 patch adds the dependency to depends_on
  bool found_add_dep = false;
  for (const auto& op : fix.patch) {
    if (op.value("op", "") == "add" &&
        op.value("path", "").find("/depends_on") != std::string::npos &&
        op.value("value", "") == "custom_prompt") {
      found_add_dep = true;
      break;
    }
  }
  EXPECT_TRUE(found_add_dep);

  // Applying patch recovers a fully valid pipeline
  const auto patched = root.patch(fix.patch);
  const auto verified_report = PipelineValidator::Validate(patched);
  EXPECT_TRUE(verified_report.ok) << verified_report.ToJson().dump(2);
}

TEST(PipelineValidatorTest, ExplainReturnsCandidateFixForUnknownConfigField) {
  std::ifstream stream(
      "demo/fixtures/mock/pipeline_entity_extract_custom.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json root;
  stream >> root;

  // Misspell "temperature" as "temprature"
  root["pipeline"][0]["config"]["temprature"] = 0.1;
  root["pipeline"][0]["config"].erase("temperature");

  const auto report = PipelineValidator::Explain(root);
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
  EXPECT_EQ(rem.schema_version, 1);
  EXPECT_EQ(rem.cause, "unknown_config_field");
  EXPECT_EQ(rem.facts.value("field", ""), "temprature");

  ASSERT_TRUE(rem.facts.contains("candidate_fields"));
  const auto& candidate_fields = rem.facts["candidate_fields"];
  ASSERT_FALSE(candidate_fields.empty());
  // The closest candidate "temperature" (Levenshtein distance 1) should be
  // first
  EXPECT_EQ(candidate_fields[0], "temperature");

  ASSERT_FALSE(rem.fixes.empty());
  const auto& fix = rem.fixes.front();
  EXPECT_EQ(fix.verification, "pipeline_valid");

  // Verify the patch moves the field from "temprature" to "temperature"
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

  // Applying patch recovers a fully valid pipeline
  const auto patched = root.patch(fix.patch);
  const auto verified_report = PipelineValidator::Validate(patched);
  EXPECT_TRUE(verified_report.ok) << verified_report.ToJson().dump(2);
}

TEST(PipelineValidatorTest, ExplainRespectsFixBounds) {
  // Construct a pipeline with 5 independent upstream nodes and 4 consumer nodes
  // having invalid dependencies. Without bounds, each consumer would produce 8
  // candidate fixes, yielding 32 candidates total.
  // Explain must cap at max 3 fixes per diagnostic and max 8 fixes per report.
  nlohmann::json root = {{"biz_name", "keyword_match_v1"},
                         {"models", nlohmann::json::array()},
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

  const auto report = PipelineValidator::Explain(root);
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

  const auto report = PipelineValidator::Explain(root);
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

  // Introduce two independent errors:
  // 1. Misspelled config field in custom_prompt ("temprature" instead of
  // "temperature")
  root["pipeline"][0]["config"]["temprature"] = 0.1;
  root["pipeline"][0]["config"].erase("temperature");

  // 2. Invalid configuration in node_2_StructuredJsonParseNode that will remain
  // unresolved
  root["pipeline"][1]["config"]["field_types"] = {
      {"field_x", "unsupported_type"}};

  const auto report = PipelineValidator::Explain(root);
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

  // Applying the fix only resolves the unknown config field; the invalid
  // field_types error in node 1 persists. Therefore, verification must be
  // "target_resolved".
  const auto& fix = typo_diag->remediation->fixes.front();
  EXPECT_EQ(fix.verification, "target_resolved");
}

TEST(PipelineValidatorTest, ValidateProducesBasicRemediation) {
  std::ifstream stream(
      "demo/fixtures/mock/pipeline_entity_extract_custom.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json root;
  stream >> root;

  // Clear depends_on so node 1 has a missing input producer
  root["pipeline"][1]["depends_on"] = nlohmann::json::array();

  const auto report = PipelineValidator::Validate(root);
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
  EXPECT_EQ(target_diag->remediation->schema_version, 1);
  EXPECT_FALSE(target_diag->remediation->cause.empty());
  EXPECT_FALSE(target_diag->remediation->summary.empty());
  EXPECT_FALSE(target_diag->remediation->facts.empty());
  EXPECT_TRUE(target_diag->remediation->fixes.empty());
}

TEST(PipelineValidatorTest, ExplainCapsVerificationAttemptsAtEight) {
  nlohmann::json root = {
      {"biz_name", "entity_extract_v1"},
      {"models", nlohmann::json::array()},
      {"pipeline",
       {{{"id", "custom_prompt"},
         {"node_type", "PromptGuidedLlmNode"},
         {"depends_on", nlohmann::json::array()},
         {"ports",
          {{"inputs", {{"input", "input_sentences"}}},
           {"outputs", {{"output", "llm_raw_answer"}}}}},
         {"config",
          {{"bind_model", "missing_model"},
           {"template_syntax", "standard"},
           {"prompt_template", "Hello {{input}}"},
           {"strip_markdown", true},
           {"temperature", 0.1},
           {"max_tokens", 64}}}},
        {{"id", "node_2_StructuredJsonParseNode"},
         {"node_type", "StructuredJsonParseNode"},
         {"depends_on", {"custom_prompt"}},
         {"ports",
          {{"inputs", {{"text", "llm_raw_answer"}}},
           {"outputs", {{"document", "extracted_entities"}}}}},
         {"config", {{"failure_policy", "fail"}}}}}}};

  for (int i = 0; i < 10; ++i) {
    char name_buf[32];
    std::snprintf(name_buf, sizeof(name_buf), "a_invalid_%02d", i);
    root["models"].push_back({
        {"model_id", name_buf},
        {"capability", "llm"},
        {"model_type", "test_biz_llm"},
        {"backend", "unknown_backend"},
        {"model_path", "demo/fixtures/mock/artifacts/neutral-llm.fixture"},
        {"model_config", {{"max_batch_size", 2}, {"max_seq_len", 512}}},
        {"backend_config", nlohmann::json::object()},
    });
  }

  root["models"].push_back({
      {"model_id", "z_valid"},
      {"capability", "llm"},
      {"model_type", "test_biz_llm"},
      {"backend", "test_causal_lm_backend"},
      {"model_path", "demo/fixtures/mock/artifacts/neutral-llm.fixture"},
      {"model_config", {{"max_batch_size", 2}, {"max_seq_len", 512}}},
      {"backend_config", nlohmann::json::object()},
  });

  const auto report = PipelineValidator::Explain(root);
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
  nlohmann::json root = {{"biz_name", "entity_extract_v1"},
                         {"models",
                          {{{"model_id", "broken_model"},
                            {"capability", "llm"},
                            {"model_type", "bge_embedding"},
                            {"backend", "test_causal_lm_backend"},
                            {"model_path", "fixture.bin"},
                            {"model_config", {{"max_batch_size", -1}}},
                            {"backend_config", nlohmann::json::object()}}}},
                         {"pipeline",
                          {{{"id", "llm_node"},
                            {"node_type", "PromptGuidedLlmNode"},
                            {"depends_on", nlohmann::json::array()},
                            {"ports",
                             {{"inputs", {{"input", "input_sentences"}}},
                              {"outputs", {{"output", "llm_raw_answer"}}}}},
                            {"config",
                             {{"bind_model", "missing_model"},
                              {"template_syntax", "standard"},
                              {"prompt_template", "Hello {{input}}"},
                              {"strip_markdown", true},
                              {"temperature", 0.1},
                              {"max_tokens", 64}}}}}}};

  const auto report = PipelineValidator::Explain(root);
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

TEST(PipelineValidatorTest, ExplainReturnsPortFlowMismatchRemediation) {
  nlohmann::json root = {
      {"biz_name", "keyword_match_v1"},
      {"models", nlohmann::json::array()},
      {"pipeline",
       {{{"id", "producer"},
         {"node_type", "TextCorpusSourceNode"},
         {"depends_on", nlohmann::json::array()},
         {"ports", {{"outputs", {{"corpus", "corpus_text"}}}}},
         {"config", {{"corpus", {"sample text"}}}}},
        {{"id", "consumer"},
         {"node_type", "TextTemplateNode"},
         {"depends_on", {"producer"}},
         {"ports",
          {{"inputs", {{"primary", "corpus_text"}}},
           {"outputs", {{"text", "rendered_text"}}}}},
         {"config", {{"template", "prefix: {{primary}}"}}}}}}};

  const auto report = PipelineValidator::Validate(root);
  EXPECT_FALSE(report.ok);
  ASSERT_FALSE(report.diagnostics.empty());

  const ValidationDiagnostic* target_diag = nullptr;
  for (const auto& diag : report.diagnostics) {
    if (diag.code == DiagnosticCode::kPortCardinalityMismatch &&
        diag.node_id == "consumer" && diag.port == "primary") {
      target_diag = &diag;
      break;
    }
  }

  ASSERT_NE(target_diag, nullptr);
  ASSERT_TRUE(target_diag->remediation.has_value());
  EXPECT_EQ(target_diag->remediation->cause, "port_flow_mismatch");
  EXPECT_EQ(target_diag->remediation->facts.value("bound_key", ""),
            "corpus_text");
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
}

TEST(PipelineValidatorTest,
     ExplainHandlesMultipleDifferentDuplicateDependencies) {
  nlohmann::json root = {
      {"biz_name", "keyword_match_v1"},
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

  const auto report = PipelineValidator::Explain(root);
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

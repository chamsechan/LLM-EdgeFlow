#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "adapter/shared_algorithm_runtime.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"

namespace alg_framework {
namespace {

class StudioCatalogProbeNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "StudioCatalogProbeNode";
  bool Init(const nlohmann::json&, SessionContext*) override { return true; }
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
  for (const auto& node_type : NodeFactory::Instance().ListTypes()) {
    EXPECT_NE(PipelineCatalog::FindNode(node_type), nullptr) << node_type;
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
  const auto* definition = PipelineCatalog::FindNode("StudioCatalogProbeNode");
  ASSERT_NE(definition, nullptr);
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
  context.Set(key, std::vector<std::string>{"a", "b"});
  ASSERT_TRUE(context.Has(key));
  ASSERT_NE(context.Get(key), nullptr);
  EXPECT_EQ(*context.Get(key), (std::vector<std::string>{"a", "b"}));
  context.Erase(key);
  EXPECT_FALSE(context.Has(key));
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

TEST(PipelineValidatorTest, ReportsCycle) {
  const nlohmann::json pipeline = {{"business_name", "keyword_match_v1"},
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
  const nlohmann::json pipeline = {{"business_name", "keyword_match_v1"},
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
            DiagnosticCode::kInvalidDependency);
  EXPECT_EQ(report.diagnostics.front().path, "/pipeline/1/depends_on/1");
}

TEST(PipelineValidatorTest, ReportsConfigAndCapabilityErrors) {
  const nlohmann::json pipeline = {
      {"business_name", "entity_extract_0.6b_v1"},
      {"models",
       {{{"model_id", "llm_model_v1"},
         {"capability", "embedding"},
         {"model_type", "test_business_embedding"},
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
      "tests/fixtures/pipeline_validation/invalid_pipeline_cases.json");
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

}  // namespace
}  // namespace alg_framework

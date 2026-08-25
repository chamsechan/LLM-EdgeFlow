#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"
#include "engine/engine_registry.h"

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
  definition.business_names = {"keyword_match_v1"};
  return definition;
}

REGISTER_NODE_WITH_DEFINITION(StudioCatalogProbeNode,
                              StudioCatalogProbeDefinition());

TEST(PipelineCatalogTest, RegisteredProductionTypesHaveDefinitions) {
  for (const auto& node_type : NodeFactory::Instance().ListTypes()) {
    EXPECT_NE(PipelineCatalog::FindNode(node_type), nullptr) << node_type;
  }
  for (const auto& engine_type : EngineFactory::Instance().ListTypes()) {
    EXPECT_NE(PipelineCatalog::FindEngine(engine_type), nullptr) << engine_type;
  }
}

TEST(PipelineCatalogTest, OutputIsDeterministicAndConflictFree) {
  EXPECT_EQ(PipelineCatalog::ToJson(), PipelineCatalog::ToJson());
  std::set<std::string> node_types;
  for (const auto& definition : PipelineCatalog::Nodes()) {
    EXPECT_TRUE(node_types.insert(definition.node_type).second)
        << definition.node_type;
  }
  std::set<std::string> engine_types;
  for (const auto& definition : PipelineCatalog::Engines()) {
    EXPECT_TRUE(engine_types.insert(definition.engine_type).second)
        << definition.engine_type;
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
  for (const auto& entry : std::filesystem::directory_iterator(configs)) {
    const auto filename = entry.path().filename().string();
    if (!entry.is_regular_file() || entry.path().extension() != ".json" ||
        filename.rfind("pipeline_", 0) != 0) {
      continue;
    }
    std::ifstream stream(entry.path());
    ASSERT_TRUE(stream.is_open()) << entry.path();
    nlohmann::json pipeline;
    ASSERT_NO_THROW(stream >> pipeline) << entry.path();
    const auto report = PipelineValidator::Validate(pipeline);
    EXPECT_TRUE(report.ok) << entry.path() << "\n" << report.ToJson().dump(2);
    ++validated;
  }
  EXPECT_GE(validated, 10U);
}

TEST(PipelineValidatorTest, ReportsCycle) {
  const nlohmann::json pipeline = {{"business_name", "keyword_match_v1"},
                                   {"pipeline",
                                    {{{"id", "a"},
                                      {"node_type", "KeywordMatcherNode"},
                                      {"depends_on", {"b"}}},
                                     {{"id", "b"},
                                      {"node_type", "KeywordMatcherNode"},
                                      {"depends_on", {"a"}}}}}};
  const auto report = PipelineValidator::Validate(pipeline);
  EXPECT_FALSE(report.ok);
  std::set<std::string> codes;
  for (const auto& diagnostic : report.diagnostics)
    codes.insert(diagnostic.code);
  EXPECT_TRUE(codes.count("DAG_CYCLE"));
}

TEST(PipelineValidatorTest, ReportsDuplicateEdge) {
  const nlohmann::json pipeline = {{"business_name", "keyword_match_v1"},
                                   {"pipeline",
                                    {{{"id", "a"},
                                      {"node_type", "KeywordMatcherNode"},
                                      {"depends_on", nlohmann::json::array()}},
                                     {{"id", "b"},
                                      {"node_type", "KeywordMatcherNode"},
                                      {"depends_on", {"a", "a"}}}}}};
  const auto report = PipelineValidator::Validate(pipeline);
  ASSERT_FALSE(report.ok);
  ASSERT_FALSE(report.diagnostics.empty());
  EXPECT_EQ(report.diagnostics.front().code, "INVALID_DEPENDENCY");
  EXPECT_EQ(report.diagnostics.front().path, "/pipeline/1/depends_on/1");
}

TEST(PipelineValidatorTest, ReportsConfigAndCapabilityErrors) {
  const nlohmann::json pipeline = {
      {"business_name", "entity_extract_0.6b_v1"},
      {"models",
       {{{"model_id", "llm_model_v1"}, {"engine_type", "mock_npu_embedding"}}}},
      {"pipeline",
       {{{"id", "pre"},
         {"node_type", "EntityExtractPreNode"},
         {"depends_on", nlohmann::json::array()}},
        {{"id", "llm"},
         {"node_type", "LlmGenerateNode"},
         {"depends_on", {"pre"}},
         {"config", {{"max_tokens", 0}, {"invented", true}}}},
        {{"id", "post"},
         {"node_type", "EntityExtractPostNode"},
         {"depends_on", {"llm"}}}}}};
  const auto report = PipelineValidator::Validate(pipeline);
  EXPECT_FALSE(report.ok);
  std::set<std::string> codes;
  for (const auto& diagnostic : report.diagnostics)
    codes.insert(diagnostic.code);
  EXPECT_TRUE(codes.count("UNKNOWN_CONFIG_FIELD"));
  EXPECT_TRUE(codes.count("CONFIG_FIELD_RANGE"));
  EXPECT_TRUE(codes.count("MODEL_CAPABILITY_MISMATCH"));
}

TEST(PipelineValidatorTest, NormalizesLegacySequenceToExplicitDag) {
  const nlohmann::json legacy = {{"business_name", "entity_extract_0.6b_v1"},
                                 {"pipeline",
                                  {{{"node_type", "EntityExtractPreNode"}},
                                   {{"node_type", "LlmGenerateNode"}},
                                   {{"node_type", "EntityExtractPostNode"}}}}};
  nlohmann::json normalized;
  ASSERT_TRUE(PipelineValidator::NormalizeExplicitDag(legacy, &normalized));
  ASSERT_EQ(normalized["pipeline"].size(), 3U);
  EXPECT_TRUE(normalized["pipeline"][0]["depends_on"].empty());
  EXPECT_EQ(normalized["pipeline"][1]["depends_on"][0],
            normalized["pipeline"][0]["id"]);
  EXPECT_EQ(normalized["pipeline"][2]["depends_on"][0],
            normalized["pipeline"][1]["id"]);
}

}  // namespace
}  // namespace alg_framework

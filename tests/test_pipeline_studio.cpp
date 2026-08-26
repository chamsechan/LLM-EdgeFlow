#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "core/node_registry.h"
#include "core/pipeline.h"
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

class StudioSchemaProbeNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "StudioSchemaProbeNode";
  bool Init(const nlohmann::json&, SessionContext*) override { return true; }
  int Process(AlgContext*) override { return 0; }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};

NodeDefinition StudioSchemaProbeDefinition() {
  NodeDefinition def;
  def.node_type = StudioSchemaProbeNode::kNodeType;
  def.category = "test";
  def.description = "Schema probe for studio test";
  def.parallel_safe = true;
  def.config_fields = {
      ConfigFieldDefinition{"req_str", ConfigValueKind::kString, /*required=*/true},
      ConfigFieldDefinition{"enum_mode", ConfigValueKind::kString, /*required=*/false,
                            /*default_value=*/"fast", /*minimum=*/std::nullopt,
                            /*maximum=*/std::nullopt,
                            /*enum_values=*/{"fast", "accurate"}},
  };
  return def;
}

REGISTER_NODE_WITH_DEFINITION(StudioSchemaProbeNode,
                              StudioSchemaProbeDefinition());

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
  std::set<DiagnosticCode> codes;
  for (const auto& diagnostic : report.diagnostics)
    codes.insert(diagnostic.code);
  EXPECT_TRUE(codes.count(DiagnosticCode::kDagCycle));
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
  EXPECT_EQ(report.diagnostics.front().code,
            DiagnosticCode::kInvalidDependency);
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
  struct ParityCase {
    const char* description;
    nlohmann::json config;
    ValidationPolicy policy;
    DiagnosticCode expected_diag;
    PipelineErrorCode expected_pipeline_error;
  };

  const std::vector<ParityCase> cases = {
      // 1. 未知业务
      {
          "Unknown business",
          {{"business_name", "invented_unknown_biz"},
           {"models", nlohmann::json::array()},
           {"pipeline",
            {{{"id", "node_0"},
              {"node_type", "KeywordMatcherNode"},
              {"depends_on", nlohmann::json::array()}}}}},
          ValidationPolicy::kStrict,
          DiagnosticCode::kUnknownBusiness,
          PipelineErrorCode::kInvalidCombination,
      },
      // 2. 未知节点
      {
          "Unknown node",
          {{"business_name", "keyword_match_v1"},
           {"models", nlohmann::json::array()},
           {"pipeline",
            {{{"id", "node_0"},
              {"node_type", "CompletelyInventedNode"},
              {"depends_on", nlohmann::json::array()}}}}},
          ValidationPolicy::kStrict,
          DiagnosticCode::kUnknownNodeType,
          PipelineErrorCode::kUnknownNodeType,
      },
      // 3. 缺失 required 配置字段 (StudioSchemaProbeNode has required req_str)
      {
          "Missing required field",
          {{"business_name", "unregistered_test_biz"},
           {"models", nlohmann::json::array()},
           {"pipeline",
            {{{"id", "node_0"},
              {"node_type", "StudioSchemaProbeNode"},
              {"depends_on", nlohmann::json::array()},
              {"config", nlohmann::json::object()}}}}},
          ValidationPolicy::kPrivateExtensionCompatible,
          DiagnosticCode::kMissingConfigField,
          PipelineErrorCode::kMissingField,
      },
      // 4. enum 枚举值错误 (StudioSchemaProbeNode enum_mode)
      {
          "Enum value error",
          {{"business_name", "unregistered_test_biz"},
           {"models", nlohmann::json::array()},
           {"pipeline",
            {{{"id", "node_0"},
              {"node_type", "StudioSchemaProbeNode"},
              {"depends_on", nlohmann::json::array()},
              {"config",
               {{"req_str", "valid"}, {"enum_mode", "bad_option"}}}}}}},
          ValidationPolicy::kPrivateExtensionCompatible,
          DiagnosticCode::kConfigFieldEnum,
          PipelineErrorCode::kInvalidCombination,
      },
      // 5. 模型能力不匹配
      {
          "Model capability mismatch",
          {{"business_name", "entity_extract_0.6b_v1"},
           {"models",
            {{{"model_id", "emb_model"},
              {"engine_type", "mock_npu_embedding"}}}},
           {"pipeline",
            {{{"id", "pre"},
              {"node_type", "EntityExtractPreNode"},
              {"depends_on", nlohmann::json::array()}},
             {{"id", "llm"},
              {"node_type", "LlmGenerateNode"},
              {"depends_on", {"pre"}},
              {"config", {{"bind_model", "emb_model"}}}},
             {{"id", "post"},
              {"node_type", "EntityExtractPostNode"},
              {"depends_on", {"llm"}}}}}},
          ValidationPolicy::kStrict,
          DiagnosticCode::kModelCapabilityMismatch,
          PipelineErrorCode::kInvalidCombination,
      },
      // 6. DAG 环
      {
          "DAG cycle",
          {{"business_name", "keyword_match_v1"},
           {"pipeline",
            {{{"id", "a"},
              {"node_type", "KeywordMatcherNode"},
              {"depends_on", {"b"}}},
             {{"id", "b"},
              {"node_type", "KeywordMatcherNode"},
              {"depends_on", {"a"}}}}}},
          ValidationPolicy::kStrict,
          DiagnosticCode::kDagCycle,
          PipelineErrorCode::kDagCycle,
      },
      // 7. 缺失输入 Producer (DocQaPostNode requires answer_text and query_text)
      {
          "Missing input producer",
          {{"business_name", "smart_doc_qa_v1"},
           {"models", nlohmann::json::array()},
           {"pipeline",
            {{{"id", "post_only"},
              {"node_type", "DocQaPostNode"},
              {"depends_on", nlohmann::json::array()}}}}},
          ValidationPolicy::kStrict,
          DiagnosticCode::kMissingInputProducer,
          PipelineErrorCode::kInvalidCombination,
      },
      // 8. 并行写冲突
      {
          "Parallel write conflict",
          {{"business_name", "unregistered_test_biz"},
           {"execution_mode", "parallel"},
           {"pipeline",
            {{{"id", "kw1"},
              {"node_type", "KeywordMatcherNode"},
              {"depends_on", nlohmann::json::array()}},
             {{"id", "kw2"},
              {"node_type", "KeywordMatcherNode"},
              {"depends_on", nlohmann::json::array()}}}}},
          ValidationPolicy::kPrivateExtensionCompatible,
          DiagnosticCode::kParallelWriteConflict,
          PipelineErrorCode::kInvalidCombination,
      },
  };

  for (const auto& test : cases) {
    SCOPED_TRACE(test.description);

    // 1. Validator 基准报告
    auto plan = PipelineValidator::ValidateAndPlan(test.config, test.policy);
    EXPECT_FALSE(plan.report.ok);
    ASSERT_FALSE(plan.report.diagnostics.empty());
    bool found_diag =
        std::any_of(plan.report.diagnostics.begin(),
                    plan.report.diagnostics.end(), [&](const auto& item) {
                      return item.code == test.expected_diag;
                    });
    EXPECT_TRUE(found_diag)
        << "Expected diagnostic " << DiagnosticCodeName(test.expected_diag)
        << " not found for case: " << test.description;

    // 2. JSON 序列化 code 检查
    auto json_report = plan.report.ToJson();
    bool found_json = false;
    for (const auto& item : json_report["diagnostics"]) {
      if (item["code"] == DiagnosticCodeName(test.expected_diag)) {
        found_json = true;
        break;
      }
    }
    EXPECT_TRUE(found_json);

    // 3. Pipeline::BuildFromJson 映射与失败状态
    Pipeline pipeline;
    PipelineDiagnostic pipe_diag;
    bool built = pipeline.BuildFromJson(test.config, &pipe_diag, test.policy);
    EXPECT_FALSE(built);
    EXPECT_EQ(pipeline.GetState(), Pipeline::State::kFailed);
  }
}

}  // namespace
}  // namespace alg_framework

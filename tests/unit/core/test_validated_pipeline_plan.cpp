#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/node_interface.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"
#include "engine/backend_registry.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"
#include "tests/support/pipeline_test_utils.h"

namespace llm_edgeflow {

class PlanTestNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "PlanTestNode";
  inline static nlohmann::json observed_config = nlohmann::json::object();

  bool Init(const NodeInitContext& init_ctx) override {
    if (!init_ctx.plan) return false;
    observed_config = init_ctx.plan->normalized_config;
    return true;
  }
  int Process(AlgContext*) override { return 0; }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};

NodeDefinition MakePlanTestNodeDefinition() {
  NodeDefinition def;
  def.node_type = PlanTestNode::kNodeType;
  def.category = "test";
  def.description = "Plan test node";
  def.config_fields = {{"retry_limit", ConfigValueKind::kInteger,
                        /*required=*/false, /*default_value=*/3,
                        /*minimum=*/0.0, /*maximum=*/10.0}};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(PlanTestNode, MakePlanTestNodeDefinition());

class IoBoundaryTestNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "IoBoundaryTestNode";
  bool Init(const NodeInitContext&) override { return true; }
  int Process(AlgContext*) override { return 0; }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};

NodeDefinition MakeIoBoundaryTestNodeDefinition() {
  NodeDefinition def;
  def.node_type = IoBoundaryTestNode::kNodeType;
  def.category = "test";
  def.description = "IO Boundary test node";
  def.inputs = {NodePortDefinition{"input_data", "TextBatch", true, "1:1",
                                   "preserve", "request"}};
  def.outputs = {NodePortDefinition{"output_data", "TextBatch", true, "1:1",
                                    "preserve", "request"}};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(IoBoundaryTestNode,
                              MakeIoBoundaryTestNodeDefinition());

class SerializedPlanTestModel : public IModel {
 public:
  inline static constexpr char kModelType[] = "serialized_plan_test";

  static std::shared_ptr<IModel> Create(const ModelCreateContext&,
                                        std::string*) {
    return std::make_shared<SerializedPlanTestModel>();
  }
  const std::string& ModelType() const noexcept override {
    static const std::string type = kModelType;
    return type;
  }
  const std::string& Capability() const noexcept override {
    static const std::string capability = "plan_test";
    return capability;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
};

ModelDefinition MakeSerializedPlanTestModelDefinition() {
  ModelDefinition def;
  def.model_type = SerializedPlanTestModel::kModelType;
  def.capability = "plan_test";
  def.description = "Serialized model used by plan validation tests";
  def.required_protocol = ExecutionProtocol::kTensorGraph;
  def.concurrency = InferenceConcurrency::kSerialized;
  return def;
}

REGISTER_MODEL_WITH_DEFINITION(SerializedPlanTestModel,
                               MakeSerializedPlanTestModelDefinition());

class ModelBoundPlanTestNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "ModelBoundPlanTestNode";
  bool Init(const NodeInitContext&) override { return true; }
  int Process(AlgContext*) override { return 0; }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};

NodeDefinition MakeModelBoundPlanTestNodeDefinition() {
  NodeDefinition def;
  def.node_type = ModelBoundPlanTestNode::kNodeType;
  def.category = "test";
  def.description = "Model-bound plan test node";
  def.config_fields = {{"bind_model", ConfigValueKind::kString, true}};
  def.model_dependencies = {{"model", "plan_test", "bind_model"}};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(ModelBoundPlanTestNode,
                              MakeModelBoundPlanTestNodeDefinition());

class MultiModelPlanTestNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "MultiModelPlanTestNode";
  bool Init(const NodeInitContext&) override { return true; }
  int Process(AlgContext*) override { return 0; }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};

NodeDefinition MakeMultiModelPlanTestNodeDefinition() {
  NodeDefinition def;
  def.node_type = MultiModelPlanTestNode::kNodeType;
  def.category = "test";
  def.description = "Multi-model plan test node";
  def.config_fields = {
      {"bind_generator", ConfigValueKind::kString, true},
      {"bind_reviewer", ConfigValueKind::kString, true},
  };
  def.model_dependencies = {
      {"generator", "plan_test", "bind_generator"},
      {"reviewer", "plan_test", "bind_reviewer"},
  };
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(MultiModelPlanTestNode,
                              MakeMultiModelPlanTestNodeDefinition());

class FlowContractProducerNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "FlowContractProducerNode";
  bool Init(const NodeInitContext&) override { return true; }
  int Process(AlgContext*) override { return 0; }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};

class FlowContractConsumerNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "FlowContractConsumerNode";
  bool Init(const NodeInitContext&) override { return true; }
  int Process(AlgContext*) override { return 0; }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};

NodeDefinition MakeFlowContractProducerDefinition() {
  NodeDefinition def;
  def.node_type = FlowContractProducerNode::kNodeType;
  def.category = "test";
  def.description = "Produces a generated request-scoped collection";
  def.outputs = {NodePortDefinition{"flow", "TextBatch", true, "1:N",
                                    "generate_sub_id", "request"}};
  return def;
}

NodeDefinition MakeFlowContractConsumerDefinition() {
  NodeDefinition def;
  def.node_type = FlowContractConsumerNode::kNodeType;
  def.category = "test";
  def.description = "Requires incompatible flow metadata";
  def.inputs = {NodePortDefinition{"flow", "TextBatch", true, "1:1",
                                   "independent", "session"}};
  return def;
}

REGISTER_NODE_WITH_DEFINITION(FlowContractProducerNode,
                              MakeFlowContractProducerDefinition());
REGISTER_NODE_WITH_DEFINITION(FlowContractConsumerNode,
                              MakeFlowContractConsumerDefinition());

class ValidatedPipelinePlanTest : public ::testing::Test {
 protected:
  void SetUp() override { RegisterTestBizs({"plan_fixture_biz"}); }
};

TEST_F(ValidatedPipelinePlanTest, DiagnosticCodeNameTableDriven) {
  struct Case {
    DiagnosticCode code;
    const char* expected_name;
  };
  const std::vector<Case> cases = {
      {DiagnosticCode::kOk, "OK"},
      {DiagnosticCode::kJsonParse, "JSON_PARSE"},
      {DiagnosticCode::kConfigFileOpen, "CONFIG_FILE_OPEN"},
      {DiagnosticCode::kRootType, "ROOT_TYPE"},
      {DiagnosticCode::kUnknownField, "UNKNOWN_FIELD"},
      {DiagnosticCode::kMissingField, "MISSING_FIELD"},
      {DiagnosticCode::kFieldType, "FIELD_TYPE"},
      {DiagnosticCode::kFieldRange, "FIELD_RANGE"},
      {DiagnosticCode::kInvalidCombination, "INVALID_COMBINATION"},
      {DiagnosticCode::kDuplicateModelId, "DUPLICATE_MODEL_ID"},
      {DiagnosticCode::kDuplicateNodeId, "DUPLICATE_NODE_ID"},
      {DiagnosticCode::kUnknownBiz, "UNKNOWN_BIZ"},
      {DiagnosticCode::kUnknownNodeType, "UNKNOWN_NODE_TYPE"},
      {DiagnosticCode::kUnknownModelType, "UNKNOWN_MODEL_TYPE"},
      {DiagnosticCode::kUnknownBackend, "UNKNOWN_BACKEND"},
      {DiagnosticCode::kModelCapabilityMismatch, "MODEL_CAPABILITY_MISMATCH"},
      {DiagnosticCode::kBackendProtocolMismatch, "BACKEND_PROTOCOL_MISMATCH"},
      {DiagnosticCode::kUnknownModelConfigField, "UNKNOWN_MODEL_CONFIG_FIELD"},
      {DiagnosticCode::kUnknownBackendConfigField,
       "UNKNOWN_BACKEND_CONFIG_FIELD"},
      {DiagnosticCode::kInvalidDependency, "INVALID_DEPENDENCY"},
      {DiagnosticCode::kDuplicateDependency, "DUPLICATE_DEPENDENCY"},
      {DiagnosticCode::kDagCycle, "DAG_CYCLE"},
      {DiagnosticCode::kRegistryConflict, "REGISTRY_CONFLICT"},
      {DiagnosticCode::kUnknownConfigField, "UNKNOWN_CONFIG_FIELD"},
      {DiagnosticCode::kMissingConfigField, "MISSING_CONFIG_FIELD"},
      {DiagnosticCode::kConfigFieldType, "CONFIG_FIELD_TYPE"},
      {DiagnosticCode::kConfigFieldRange, "CONFIG_FIELD_RANGE"},
      {DiagnosticCode::kConfigFieldEnum, "CONFIG_FIELD_ENUM"},
      {DiagnosticCode::kUnknownModelReference, "UNKNOWN_MODEL_REFERENCE"},
      {DiagnosticCode::kNodeBizMismatch, "NODE_BIZ_MISMATCH"},
      {DiagnosticCode::kMissingInputProducer, "MISSING_INPUT_PRODUCER"},
      {DiagnosticCode::kDuplicatePortProducer, "DUPLICATE_PORT_PRODUCER"},
      {DiagnosticCode::kMissingBizOutput, "MISSING_BIZ_OUTPUT"},
      {DiagnosticCode::kNodeNotParallelSafe, "NODE_NOT_PARALLEL_SAFE"},
      {DiagnosticCode::kParallelWriteConflict, "PARALLEL_WRITE_CONFLICT"},
      {DiagnosticCode::kSerializedModelConcurrency,
       "SERIALIZED_MODEL_CONCURRENCY"},
      {DiagnosticCode::kPortCardinalityMismatch, "PORT_CARDINALITY_MISMATCH"},
      {DiagnosticCode::kPortProvenanceMismatch, "PORT_PROVENANCE_MISMATCH"},
      {DiagnosticCode::kPortLifetimeMismatch, "PORT_LIFETIME_MISMATCH"},
      {DiagnosticCode::kInternalException, "INTERNAL_EXCEPTION"},
      {DiagnosticCode::kModelMaterializationFailed,
       "MODEL_MATERIALIZATION_FAILED"},
      {DiagnosticCode::kNodeCreateFailed, "NODE_CREATE_FAILED"},
      {DiagnosticCode::kNodeInitFailed, "NODE_INIT_FAILED"},
      {DiagnosticCode::kInvalidBuildState, "INVALID_BUILD_STATE"},
  };

  EXPECT_EQ(cases.size(), 44u);
  std::unordered_set<std::string> names;
  for (const auto& item : cases) {
    std::string name = DiagnosticCodeName(item.code);
    EXPECT_STREQ(name.c_str(), item.expected_name);
    EXPECT_TRUE(names.insert(name).second) << "Duplicate name: " << name;
  }
  EXPECT_STREQ(DiagnosticCodeName(static_cast<DiagnosticCode>(9999)),
               "UNKNOWN");
}

TEST_F(ValidatedPipelinePlanTest, RejectsIncompatiblePortExecutionContracts) {
  nlohmann::json pipeline_json = {
      {"biz_name", "plan_fixture_biz"},
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array(
           {{{"id", "producer"},
             {"node_type", FlowContractProducerNode::kNodeType},
             {"depends_on", nlohmann::json::array()}},
            {{"id", "consumer"},
             {"node_type", FlowContractConsumerNode::kNodeType},
             {"depends_on", nlohmann::json::array({"producer"})}}})}};

  auto plan = PipelineValidator::ValidateAndPlan(pipeline_json);
  EXPECT_FALSE(plan.report.ok);
  const std::unordered_set<DiagnosticCode> expected = {
      DiagnosticCode::kPortCardinalityMismatch,
      DiagnosticCode::kPortProvenanceMismatch,
      DiagnosticCode::kPortLifetimeMismatch};
  std::unordered_set<DiagnosticCode> actual;
  for (const auto& diagnostic : plan.report.diagnostics) {
    actual.insert(diagnostic.code);
    if (expected.count(diagnostic.code)) {
      EXPECT_EQ(diagnostic.path, "/pipeline/1/ports/inputs/flow");
      EXPECT_EQ(diagnostic.node_id, "consumer");
      EXPECT_EQ(diagnostic.port, "flow");
      EXPECT_EQ(diagnostic.related_nodes,
                std::vector<std::string>({"producer"}));
    }
  }
  for (const auto code : expected) EXPECT_TRUE(actual.count(code));
}

TEST_F(ValidatedPipelinePlanTest,
       RejectsDuplicateProducerEvenWhenDefinitionAllowsOverride) {
  nlohmann::json pipeline_json = {
      {"biz_name", "plan_fixture_biz"},
      {"models", nlohmann::json::array()},
      {"pipeline", nlohmann::json::array(
                       {{{"id", "first"},
                         {"node_type", FlowContractProducerNode::kNodeType},
                         {"depends_on", nlohmann::json::array()}},
                        {{"id", "second"},
                         {"node_type", FlowContractProducerNode::kNodeType},
                         {"depends_on", nlohmann::json::array({"first"})}}})}};

  const auto plan = PipelineValidator::ValidateAndPlan(pipeline_json);
  ASSERT_FALSE(plan.report.ok);
  const auto diagnostic =
      std::find_if(plan.report.diagnostics.begin(),
                   plan.report.diagnostics.end(), [](const auto& item) {
                     return item.code == DiagnosticCode::kDuplicatePortProducer;
                   });
  ASSERT_NE(diagnostic, plan.report.diagnostics.end());
  EXPECT_EQ(diagnostic->node_id, "second");
  EXPECT_EQ(diagnostic->port, "flow");
  EXPECT_EQ(diagnostic->related_nodes, std::vector<std::string>({"first"}));
}

TEST_F(ValidatedPipelinePlanTest, RejectsNodeOutputBoundToBusinessIngress) {
  std::ifstream stream("configs/pipeline_doc_qa_default.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json pipeline_json;
  stream >> pipeline_json;
  pipeline_json.erase("deployment");
  const size_t source_index = pipeline_json["pipeline"].size();
  pipeline_json["pipeline"].push_back(
      {{"id", "ingress_collision"},
       {"node_type", "TextChunkNode"},
       {"depends_on", nlohmann::json::array()},
       {"ports",
        {{"inputs", {{"text", "raw_docs"}}},
         {"outputs",
          {{"chunks", "raw_docs"}, {"chunk_counts", "audit_counts"}}}}},
       {"config", {{"chunk_size", 60}}}});

  const auto plan = PipelineValidator::ValidateAndPlan(pipeline_json);
  ASSERT_FALSE(plan.report.ok);
  const auto diagnostic =
      std::find_if(plan.report.diagnostics.begin(),
                   plan.report.diagnostics.end(), [](const auto& item) {
                     return item.code == DiagnosticCode::kDuplicatePortProducer;
                   });
  ASSERT_NE(diagnostic, plan.report.diagnostics.end());
  EXPECT_EQ(diagnostic->path, "/pipeline/" + std::to_string(source_index) +
                                  "/ports/outputs/chunks");
  EXPECT_EQ(diagnostic->node_id, "ingress_collision");
  EXPECT_EQ(diagnostic->port, "chunks");
  EXPECT_EQ(diagnostic->related_nodes, std::vector<std::string>({"$ingress"}));
}

TEST_F(ValidatedPipelinePlanTest,
       ResolvesConfiguredPortLifetimeBeforePlanning) {
  nlohmann::json pipeline_json = {
      {"biz_name", "smart_doc_qa_v1"},
      {"models",
       nlohmann::json::array({{{"model_id", "embed_model_v1"},
                               {"capability", "embedding"},
                               {"model_type", "test_biz_embedding"},
                               {"backend", "test_tensor_backend"},
                               {"model_path", "fixture.bin"},
                               {"model_config", nlohmann::json::object()},
                               {"backend_config", nlohmann::json::object()}}})},
      {"pipeline",
       nlohmann::json::array(
           {{{"id", "session_embedding"},
             {"node_type", "TextEmbeddingNode"},
             {"depends_on", nlohmann::json::array()},
             {"ports", {{"inputs", {{"text", "raw_queries"}}}}},
             {"config",
              {{"bind_model", "embed_model_v1"}, {"lifetime", "session"}}}}})}};

  auto plan = PipelineValidator::ValidateAndPlan(pipeline_json);
  auto diagnostic =
      std::find_if(plan.report.diagnostics.begin(),
                   plan.report.diagnostics.end(), [](const auto& item) {
                     return item.code == DiagnosticCode::kPortLifetimeMismatch;
                   });
  ASSERT_NE(diagnostic, plan.report.diagnostics.end());
  EXPECT_EQ(diagnostic->node_id, "session_embedding");
  const auto* binding = plan.node_plans.at("session_embedding")
                            .FindPort("text", PortDirection::kInput);
  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(binding->lifetime, "session");
}

TEST_F(ValidatedPipelinePlanTest, UnknownBusinessFailsClosed) {
  auto plan = PipelineValidator::ValidateAndPlan(
      {{"biz_name", "unknown_plan_biz"},
       {"pipeline",
        {{{"id", "node"},
          {"node_type", "PlanTestNode"},
          {"depends_on", nlohmann::json::array()}}}}});
  ASSERT_FALSE(plan.report.ok);
  ASSERT_FALSE(plan.report.diagnostics.empty());
  EXPECT_EQ(plan.report.diagnostics.front().code, DiagnosticCode::kUnknownBiz);
}

TEST_F(ValidatedPipelinePlanTest, NormalizedNodeConfigIsRuntimeSingleSource) {
  nlohmann::json pipeline_json = {
      {"biz_name", "plan_fixture_biz"},
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_0"},
                               {"node_type", PlanTestNode::kNodeType},
                               {"depends_on", nlohmann::json::array()}}})}};

  auto plan = PipelineValidator::ValidateAndPlan(pipeline_json);
  ASSERT_TRUE(plan.report.ok);
  ASSERT_TRUE(plan.node_plans.at("node_0").node.config.empty());
  EXPECT_EQ(plan.node_plans.at("node_0").normalized_config.at("retry_limit"),
            3);

  PlanTestNode::observed_config = nlohmann::json::object();
  Pipeline pipeline;
  PipelineDiagnostic diagnostic;
  ASSERT_TRUE(BuildTestPipeline(pipeline, pipeline_json, &diagnostic))
      << diagnostic.message;
  EXPECT_EQ(PlanTestNode::observed_config.at("retry_limit"), 3);
  EXPECT_EQ(pipeline.GetBizName(), "plan_fixture_biz");
  EXPECT_EQ(pipeline.GetTopologicalOrder(),
            std::vector<std::string>({"node_0"}));
}

TEST_F(ValidatedPipelinePlanTest, MultiLayerWavefrontTopology) {
  // Test DAG Wavefront layers calculation
  nlohmann::json dag_json = {
      {"biz_name", "plan_fixture_biz"},
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array({
           {{"id", "node_a"},
            {"node_type", "PlanTestNode"},
            {"depends_on", nlohmann::json::array()}},
           {{"id", "node_b"},
            {"node_type", "PlanTestNode"},
            {"depends_on", nlohmann::json::array()}},
           {{"id", "node_c"},
            {"node_type", "PlanTestNode"},
            {"depends_on", nlohmann::json::array({"node_a", "node_b"})}},
       })}};

  auto plan = PipelineValidator::ValidateAndPlan(dag_json);
  EXPECT_TRUE(plan.report.ok);
  ASSERT_EQ(plan.report.topological_layers.size(), 2u);
  // Layer 0 has node_a and node_b
  EXPECT_EQ(plan.report.topological_layers[0].size(), 2u);
  // Layer 1 has node_c
  EXPECT_EQ(plan.report.topological_layers[1].size(), 1u);
  EXPECT_EQ(plan.report.topological_layers[1][0], "node_c");
}

TEST_F(ValidatedPipelinePlanTest, RejectsSharedSerializedModelInParallelLayer) {
  nlohmann::json pipeline_json = {
      {"biz_name", "plan_fixture_biz"},
      {"execution_mode", "parallel"},
      {"models", nlohmann::json::array(
                     {{{"model_id", "shared"},
                       {"capability", "plan_test"},
                       {"model_type", SerializedPlanTestModel::kModelType},
                       {"backend", "test_tensor_backend"},
                       {"model_path", "serialized.bin"},
                       {"model_config", nlohmann::json::object()},
                       {"backend_config", nlohmann::json::object()}}})},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_a"},
                               {"node_type", ModelBoundPlanTestNode::kNodeType},
                               {"depends_on", nlohmann::json::array()},
                               {"config", {{"bind_model", "shared"}}}},
                              {{"id", "node_b"},
                               {"node_type", ModelBoundPlanTestNode::kNodeType},
                               {"depends_on", nlohmann::json::array()},
                               {"config", {{"bind_model", "shared"}}}}})}};

  auto plan = PipelineValidator::ValidateAndPlan(pipeline_json);
  EXPECT_FALSE(plan.report.ok);
  auto diagnostic = std::find_if(
      plan.report.diagnostics.begin(), plan.report.diagnostics.end(),
      [](const auto& item) {
        return item.code == DiagnosticCode::kSerializedModelConcurrency;
      });
  ASSERT_NE(diagnostic, plan.report.diagnostics.end());
  EXPECT_EQ(diagnostic->node_id, "node_b");
  EXPECT_EQ(diagnostic->related_nodes, std::vector<std::string>({"node_a"}));
}

class RestrictedBusinessNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "RestrictedBusinessNode";
  bool Init(const NodeInitContext&) override { return true; }
  int Process(AlgContext*) override { return 0; }
  const std::string& Name() const override {
    static const std::string n = kNodeType;
    return n;
  }
};

inline NodeDefinition MakeRestrictedNodeDef() {
  NodeDefinition def;
  def.node_type = RestrictedBusinessNode::kNodeType;
  def.category = "biz";
  def.biz_names = {"restricted_only_biz"};
  def.description = "Restricted test node";
  return def;
}
REGISTER_NODE_WITH_DEFINITION(RestrictedBusinessNode, MakeRestrictedNodeDef());

TEST_F(ValidatedPipelinePlanTest, RejectsNodeFromDifferentBusiness) {
  nlohmann::json pipeline_json = {
      {"biz_name", "smart_doc_qa_v1"},
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array({{{"id", "wrong_business_node"},
                               {"node_type", "RestrictedBusinessNode"},
                               {"depends_on", nlohmann::json::array()}}})}};

  auto plan = PipelineValidator::ValidateAndPlan(pipeline_json);
  EXPECT_FALSE(plan.report.ok);
  EXPECT_NE(std::find_if(plan.report.diagnostics.begin(),
                         plan.report.diagnostics.end(),
                         [](const auto& item) {
                           return item.code == DiagnosticCode::kNodeBizMismatch;
                         }),
            plan.report.diagnostics.end());
}

TEST_F(ValidatedPipelinePlanTest,
       DeterministicLexicalModelPathValidationWithoutDeploymentContext) {
  if (!BackendRegistry::Instance().Find("mock_path_backend").has_value()) {
    BackendDefinition bdef;
    bdef.backend_type = "mock_path_backend";
    bdef.supported_protocols = {ExecutionProtocol::kTensorGraph};
    bdef.concurrency = InferenceConcurrency::kConcurrent;
    BackendRegistry::Instance().Register(
        bdef, []() -> std::unique_ptr<IInferenceBackend> { return nullptr; });
  }

  if (!ModelRegistry::Instance().Find("mock_path_model").has_value()) {
    ModelDefinition mdef;
    mdef.model_type = "mock_path_model";
    mdef.capability = "rerank";
    mdef.required_protocol = ExecutionProtocol::kTensorGraph;
    mdef.concurrency = InferenceConcurrency::kConcurrent;
    ModelRegistry::Instance().Register(
        mdef,
        [](const ModelCreateContext&, std::string*) -> std::shared_ptr<IModel> {
          return nullptr;
        });
  }

  nlohmann::json pipeline_json = {
      {"biz_name", "dense_cross_rerank_scoring"},
      {"models",
       nlohmann::json::array({{{"model_id", "m_rel"},
                               {"capability", "rerank"},
                               {"model_type", "mock_path_model"},
                               {"backend", "mock_path_backend"},
                               {"model_path", "./models/sub/model.onnx"},
                               {"model_config", nlohmann::json::object()}},
                              {{"model_id", "m_abs"},
                               {"capability", "rerank"},
                               {"model_type", "mock_path_model"},
                               {"backend", "mock_path_backend"},
                               {"model_path", "/opt/models/fixed.onnx"},
                               {"model_config", nlohmann::json::object()}},
                              {{"model_id", "m_direct"},
                               {"capability", "rerank"},
                               {"model_type", "mock_path_model"},
                               {"backend", "mock_path_backend"},
                               {"model_path", "model_direct.onnx"},
                               {"model_config", nlohmann::json::object()}}})},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_0_TextRerankNode"},
                               {"node_type", "TextRerankNode"},
                               {"depends_on", nlohmann::json::array()},
                               {"ports",
                                {{"inputs",
                                  {{"queries", "rerank_queries"},
                                   {"candidates", "rerank_candidates"}}},
                                 {"outputs", {{"ranked", "ranked_results"}}}}},
                               {"config", {{"bind_model", "m_rel"}}}}})}};

  // Orchestration only performs deterministic lexical normalization. Deployment
  // roots are an Integration concern.
  auto plan = PipelineValidator::ValidateAndPlan(pipeline_json);
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump();
  EXPECT_EQ(plan.models[0].resolved_model_path, "models/sub/model.onnx");
  EXPECT_EQ(plan.models[1].resolved_model_path, "/opt/models/fixed.onnx");
  EXPECT_EQ(plan.models[2].resolved_model_path, "model_direct.onnx");

  auto plan_repeat = PipelineValidator::ValidateAndPlan(pipeline_json);
  ASSERT_TRUE(plan_repeat.report.ok);
  EXPECT_EQ(plan_repeat.models[0].resolved_model_path,
            plan.models[0].resolved_model_path);
  EXPECT_EQ(plan_repeat.models[1].resolved_model_path,
            plan.models[1].resolved_model_path);
  EXPECT_EQ(plan_repeat.models[2].resolved_model_path,
            plan.models[2].resolved_model_path);

  // Parent traversal remains invalid even before deployment resolution.
  nlohmann::json escape_json = pipeline_json;
  escape_json["models"][0]["model_path"] = "../escape.onnx";
  auto plan_escape = PipelineValidator::ValidateAndPlan(escape_json);
  EXPECT_FALSE(plan_escape.report.ok);
}

TEST_F(ValidatedPipelinePlanTest,
       RejectsIncompatibleEgressPortExecutionContracts) {
  BizDefinition biz_def;
  biz_def.biz_name = "test_egress_flow_biz";
  biz_def.demo_biz = "test";
  biz_def.egress = {BizPortDefinition{"flow", "TextBatch", true, "1:1",
                                      "independent", "session"}};
  ASSERT_TRUE(PipelineCatalog::RegisterBizDefinition(biz_def));

  nlohmann::json pipeline_json = {
      {"biz_name", "test_egress_flow_biz"},
      {"models", nlohmann::json::array()},
      {"pipeline", nlohmann::json::array(
                       {{{"id", "producer"},
                         {"node_type", FlowContractProducerNode::kNodeType},
                         {"depends_on", nlohmann::json::array()}}})}};

  auto plan = PipelineValidator::ValidateAndPlan(pipeline_json);
  EXPECT_FALSE(plan.report.ok);

  bool found_cardinality = false;
  bool found_provenance = false;
  bool found_lifetime = false;
  for (const auto& diag : plan.report.diagnostics) {
    if (diag.code == DiagnosticCode::kPortCardinalityMismatch) {
      found_cardinality = true;
      EXPECT_EQ(diag.related_nodes, std::vector<std::string>({"$egress"}));
      EXPECT_EQ(diag.port, "flow");
    } else if (diag.code == DiagnosticCode::kPortProvenanceMismatch) {
      found_provenance = true;
      EXPECT_EQ(diag.related_nodes, std::vector<std::string>({"$egress"}));
      EXPECT_EQ(diag.port, "flow");
    } else if (diag.code == DiagnosticCode::kPortLifetimeMismatch) {
      found_lifetime = true;
      EXPECT_EQ(diag.related_nodes, std::vector<std::string>({"$egress"}));
      EXPECT_EQ(diag.port, "flow");
    }
  }
  EXPECT_TRUE(found_cardinality);
  EXPECT_TRUE(found_provenance);
  EXPECT_TRUE(found_lifetime);
}

TEST_F(ValidatedPipelinePlanTest,
       OptionalEgressMayBeAbsentButMustMatchWhenPresent) {
  BizDefinition biz;
  biz.biz_name = "test_optional_egress_type";
  biz.egress = {BizPortDefinition{"optional", "Int32Batch", false, "N:M",
                                  "aggregate", "request"}};
  ASSERT_TRUE(PipelineCatalog::RegisterBizDefinition(biz));
  nlohmann::json config = {
      {"biz_name", biz.biz_name},
      {"pipeline",
       {{{"id", "source"},
         {"node_type", FlowContractProducerNode::kNodeType},
         {"depends_on", nlohmann::json::array()}}}}};
  EXPECT_TRUE(PipelineValidator::ValidateAndPlan(config).report.ok);
  config["pipeline"][0]["ports"]["outputs"]["flow"] = "optional";
  auto plan = PipelineValidator::ValidateAndPlan(config);
  EXPECT_FALSE(plan.report.ok);
  ASSERT_FALSE(plan.report.diagnostics.empty());
  EXPECT_EQ(plan.report.diagnostics.back().code,
            DiagnosticCode::kMissingBizOutput);
  EXPECT_EQ(plan.report.diagnostics.back().related_nodes,
            std::vector<std::string>{"$egress"});
}

TEST_F(ValidatedPipelinePlanTest,
       MultiModelBindingsAndConcurrencyDeduplication) {
  nlohmann::json base_pipeline = {
      {"biz_name", "plan_fixture_biz"},
      {"execution_mode", "parallel"},
      {"models", nlohmann::json::array({
                     {{"model_id", "shared_a"},
                      {"model_type", SerializedPlanTestModel::kModelType},
                      {"capability", "plan_test"},
                      {"backend", "test_tensor_backend"},
                      {"model_path", "model_a.bin"},
                      {"model_config", nlohmann::json::object()},
                      {"backend_config", nlohmann::json::object()}},
                     {{"model_id", "independent_b"},
                      {"model_type", SerializedPlanTestModel::kModelType},
                      {"capability", "plan_test"},
                      {"backend", "test_tensor_backend"},
                      {"model_path", "model_b.bin"},
                      {"model_config", nlohmann::json::object()},
                      {"backend_config", nlohmann::json::object()}},
                 })},
      {"pipeline",
       nlohmann::json::array({
           {{"id", "multi_node"},
            {"node_type", MultiModelPlanTestNode::kNodeType},
            {"depends_on", nlohmann::json::array()},
            {"config",
             {{"bind_generator", "shared_a"}, {"bind_reviewer", "shared_a"}}}},
           {{"id", "parallel_node"},
            {"node_type", ModelBoundPlanTestNode::kNodeType},
            {"depends_on", nlohmann::json::array()},
            {"config", {{"bind_model", "independent_b"}}}},
       })}};

  // 1. 同节点同实例去重且独立实例并行通过
  auto plan_ok = PipelineValidator::ValidateAndPlan(base_pipeline);
  EXPECT_TRUE(plan_ok.report.ok);
  const auto& multi_plan = plan_ok.node_plans["multi_node"];
  ASSERT_EQ(multi_plan.model_bindings.size(), 2U);
  EXPECT_EQ(multi_plan.model_bindings[0].name, "generator");
  EXPECT_EQ(multi_plan.model_bindings[0].capability, "plan_test");
  EXPECT_EQ(multi_plan.model_bindings[0].config_field, "bind_generator");
  EXPECT_EQ(multi_plan.model_bindings[0].model_id, "shared_a");

  EXPECT_EQ(multi_plan.model_bindings[1].name, "reviewer");
  EXPECT_EQ(multi_plan.model_bindings[1].capability, "plan_test");
  EXPECT_EQ(multi_plan.model_bindings[1].config_field, "bind_reviewer");
  EXPECT_EQ(multi_plan.model_bindings[1].model_id, "shared_a");

  // 2. 其他节点共享第一槽位或第二槽位的 serialized 模型时被拒绝
  auto conflict_pipeline = base_pipeline;
  conflict_pipeline["pipeline"][1]["config"]["bind_model"] = "shared_a";
  auto plan_err = PipelineValidator::ValidateAndPlan(conflict_pipeline);
  EXPECT_FALSE(plan_err.report.ok);
  auto diag = std::find_if(
      plan_err.report.diagnostics.begin(), plan_err.report.diagnostics.end(),
      [](const auto& item) {
        return item.code == DiagnosticCode::kSerializedModelConcurrency;
      });
  ASSERT_NE(diag, plan_err.report.diagnostics.end());
  EXPECT_EQ(diag->node_id, "parallel_node");
  EXPECT_EQ(diag->path, "/pipeline/1/config/bind_model");
  EXPECT_EQ(diag->related_nodes, std::vector<std::string>{"multi_node"});
}

TEST_F(ValidatedPipelinePlanTest,
       IoBoundaryValidationCoversIngressEgressAndExtraWrites) {
  // 注册测试用 biz definition
  BizDefinition test_biz;
  test_biz.biz_name = "io_boundary_test_biz";
  test_biz.ingress = {
      BizPortDefinition("text_in", "TextBatch", /*required=*/true),
      BizPortDefinition("opt_in", "TextBatch", /*required=*/false)};
  test_biz.egress = {
      BizPortDefinition("text_out", "TextBatch", /*required=*/true)};
  PipelineCatalog::RegisterBizDefinition(test_biz);

  nlohmann::json valid_pipeline = {
      {"biz_name", "io_boundary_test_biz"},
      {"pipeline", nlohmann::json::array({
                       {{"id", "node1"},
                        {"node_type", "IoBoundaryTestNode"},
                        {"depends_on", nlohmann::json::array()},
                        {"ports",
                         {{"inputs", {{"input_data", "text_in"}}},
                          {"outputs", {{"output_data", "text_out"}}}}}},
                   })}};

  // 1. 合法 IO boundary：覆盖必需 ingress，消费 egress
  PipelineIoBoundary valid_boundary;
  valid_boundary.input_published_ports = {
      BizPortDefinition("text_in", "TextBatch", true)};
  valid_boundary.output_consumed_ports = {
      BizPortDefinition("text_out", "TextBatch", true)};

  auto plan_ok =
      PipelineValidator::ValidateAndPlan(valid_pipeline, &valid_boundary);
  EXPECT_TRUE(plan_ok.report.ok);

  // 2. 缺失必需 ingress 端口发布
  PipelineIoBoundary missing_in_boundary;
  missing_in_boundary.output_consumed_ports =
      valid_boundary.output_consumed_ports;
  auto plan_missing_in =
      PipelineValidator::ValidateAndPlan(valid_pipeline, &missing_in_boundary);
  EXPECT_FALSE(plan_missing_in.report.ok);
  auto diag_in =
      std::find_if(plan_missing_in.report.diagnostics.begin(),
                   plan_missing_in.report.diagnostics.end(), [](const auto& d) {
                     return d.code == DiagnosticCode::kMissingInputProducer;
                   });
  ASSERT_NE(diag_in, plan_missing_in.report.diagnostics.end());
  EXPECT_EQ(diag_in->path, "/io/input");

  // 3. 缺失输出消费者所需的生产者
  PipelineIoBoundary missing_out_boundary = valid_boundary;
  missing_out_boundary.output_consumed_ports.push_back(
      BizPortDefinition("unproduced_out", "TextBatch", true));
  auto plan_missing_out =
      PipelineValidator::ValidateAndPlan(valid_pipeline, &missing_out_boundary);
  EXPECT_FALSE(plan_missing_out.report.ok);
  auto diag_out = std::find_if(
      plan_missing_out.report.diagnostics.begin(),
      plan_missing_out.report.diagnostics.end(), [](const auto& d) {
        return d.code == DiagnosticCode::kMissingBizOutput;
      });
  ASSERT_NE(diag_out, plan_missing_out.report.diagnostics.end());
  EXPECT_EQ(diag_out->path, "/io/output");

  // 4. 输入额外发布与 Pipeline 内部节点输出冲突 (重复写入)
  PipelineIoBoundary conflict_boundary = valid_boundary;
  conflict_boundary.input_published_ports.push_back(
      BizPortDefinition("text_out", "TextBatch", true));
  auto plan_conflict =
      PipelineValidator::ValidateAndPlan(valid_pipeline, &conflict_boundary);
  EXPECT_FALSE(plan_conflict.report.ok);
  auto diag_conflict =
      std::find_if(plan_conflict.report.diagnostics.begin(),
                   plan_conflict.report.diagnostics.end(), [](const auto& d) {
                     return d.code == DiagnosticCode::kDuplicatePortProducer;
                   });
  ASSERT_NE(diag_conflict, plan_conflict.report.diagnostics.end());
}

TEST_F(ValidatedPipelinePlanTest, PipelineBuildFromPlanLifecycle) {
  nlohmann::json valid_pipeline = {
      {"biz_name", "io_boundary_test_biz"},
      {"pipeline", nlohmann::json::array({
                       {{"id", "node1"},
                        {"node_type", "IoBoundaryTestNode"},
                        {"depends_on", nlohmann::json::array()},
                        {"ports",
                         {{"inputs", {{"input_data", "text_in"}}},
                          {"outputs", {{"output_data", "text_out"}}}}}},
                   })}};

  PipelineIoBoundary boundary;
  boundary.input_published_ports = {
      BizPortDefinition("text_in", "TextBatch", true)};
  boundary.output_consumed_ports = {
      BizPortDefinition("text_out", "TextBatch", true)};

  auto plan = std::make_unique<ValidatedPipelinePlan>(
      PipelineValidator::ValidateAndPlan(valid_pipeline, &boundary));
  ASSERT_TRUE(plan->report.ok);

  Pipeline pipeline;
  EXPECT_EQ(pipeline.GetState(), Pipeline::State::kEmpty);

  PipelineDiagnostic diag;
  bool built = pipeline.BuildFromPlan(std::move(plan), &diag);
  EXPECT_TRUE(built);
  EXPECT_EQ(pipeline.GetState(), Pipeline::State::kReady);
  EXPECT_TRUE(pipeline.IsReady());

  // 重复构建应被拒绝
  auto plan2 = std::make_unique<ValidatedPipelinePlan>();
  EXPECT_FALSE(pipeline.BuildFromPlan(std::move(plan2), &diag));
  EXPECT_EQ(diag.code, DiagnosticCode::kInvalidBuildState);
}

}  // namespace llm_edgeflow

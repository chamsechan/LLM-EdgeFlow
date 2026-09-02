#include <gtest/gtest.h>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"
#include "engine/backend_registry.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {

class PlanTestNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "PlanTestNode";
  inline static nlohmann::json observed_config = nlohmann::json::object();

  bool Init(const NodeInitContext& init_ctx) override {
    if (!init_ctx.config) return false;
    observed_config = *init_ctx.config;
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

class SerializedPlanTestModel : public IModel {
 public:
  inline static constexpr char kModelType[] = "serialized_plan_test";

  static std::shared_ptr<IModel> Create(const ModelCreateContext&,
                                        std::string*) {
    return std::make_shared<SerializedPlanTestModel>();
  }
  size_t GetMaxBatchSize() const noexcept override { return 1; }
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
  def.model_capability = "plan_test";
  def.model_config_field = "bind_model";
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(ModelBoundPlanTestNode,
                              MakeModelBoundPlanTestNodeDefinition());

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
  def.outputs = {PortDefinition{"flow", "TextBatch", true, "1:N",
                                "generate_sub_id", "request"}};
  return def;
}

NodeDefinition MakeFlowContractConsumerDefinition() {
  NodeDefinition def;
  def.node_type = FlowContractConsumerNode::kNodeType;
  def.category = "test";
  def.description = "Requires incompatible flow metadata";
  def.inputs = {PortDefinition{"flow", "TextBatch", true, "1:1", "independent",
                               "session"}};
  return def;
}

REGISTER_NODE_WITH_DEFINITION(FlowContractProducerNode,
                              MakeFlowContractProducerDefinition());
REGISTER_NODE_WITH_DEFINITION(FlowContractConsumerNode,
                              MakeFlowContractConsumerDefinition());

TEST(ValidatedPipelinePlanTest, DiagnosticCodeNameTableDriven) {
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
      {DiagnosticCode::kModelCapabilityMismatch, "MODEL_CAPABILITY_MISMATCH"},
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
  };

  std::unordered_set<std::string> names;
  for (const auto& item : cases) {
    std::string name = DiagnosticCodeName(item.code);
    EXPECT_STREQ(name.c_str(), item.expected_name);
    EXPECT_TRUE(names.insert(name).second) << "Duplicate name: " << name;
  }
}

TEST(ValidatedPipelinePlanTest, RejectsIncompatiblePortExecutionContracts) {
  nlohmann::json pipeline_json = {
      {"biz_name", "unregistered_test_biz"},
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array(
           {{{"id", "producer"},
             {"node_type", FlowContractProducerNode::kNodeType},
             {"depends_on", nlohmann::json::array()}},
            {{"id", "consumer"},
             {"node_type", FlowContractConsumerNode::kNodeType},
             {"depends_on", nlohmann::json::array({"producer"})}}})}};

  auto plan = PipelineValidator::ValidateAndPlan(
      pipeline_json, ValidationPolicy::kPrivateExtensionCompatible);
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

TEST(ValidatedPipelinePlanTest,
     RejectsDuplicateProducerEvenWhenDefinitionAllowsOverride) {
  nlohmann::json pipeline_json = {
      {"biz_name", "unregistered_test_biz"},
      {"models", nlohmann::json::array()},
      {"pipeline", nlohmann::json::array(
                       {{{"id", "first"},
                         {"node_type", FlowContractProducerNode::kNodeType},
                         {"depends_on", nlohmann::json::array()}},
                        {{"id", "second"},
                         {"node_type", FlowContractProducerNode::kNodeType},
                         {"depends_on", nlohmann::json::array({"first"})}}})}};

  const auto plan = PipelineValidator::ValidateAndPlan(
      pipeline_json, ValidationPolicy::kPrivateExtensionCompatible);
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

TEST(ValidatedPipelinePlanTest, ResolvesConfiguredPortLifetimeBeforePlanning) {
  nlohmann::json pipeline_json = {
      {"biz_name", "smart_doc_qa_v1"},
      {"models",
       nlohmann::json::array({{{"model_id", "embed_model_v1"},
                               {"capability", "embedding"},
                               {"model_type", "test_business_embedding"},
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

TEST(ValidatedPipelinePlanTest, StrictVsCompatiblePolicy) {
  // 1. Unregistered business with strict policy fails
  nlohmann::json unreg_biz_json = {
      {"biz_name", "unregistered_test_biz"},
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_0"},
                               {"node_type", "PlanTestNode"},
                               {"depends_on", nlohmann::json::array()}}})}};

  auto strict_plan = PipelineValidator::ValidateAndPlan(
      unreg_biz_json, ValidationPolicy::kStrict);
  EXPECT_FALSE(strict_plan.report.ok);
  ASSERT_FALSE(strict_plan.report.diagnostics.empty());
  EXPECT_EQ(strict_plan.report.diagnostics.front().code,
            DiagnosticCode::kUnknownBiz);

  auto compat_plan = PipelineValidator::ValidateAndPlan(
      unreg_biz_json, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_TRUE(compat_plan.report.ok);
  EXPECT_EQ(compat_plan.topological_order.size(), 1u);
  EXPECT_EQ(compat_plan.topological_layers.size(), 1u);
}

TEST(ValidatedPipelinePlanTest, NormalizedNodeConfigIsRuntimeSingleSource) {
  nlohmann::json pipeline_json = {
      {"biz_name", "unregistered_test_biz"},
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_0"},
                               {"node_type", PlanTestNode::kNodeType},
                               {"depends_on", nlohmann::json::array()}}})}};

  auto plan = PipelineValidator::ValidateAndPlan(
      pipeline_json, ValidationPolicy::kPrivateExtensionCompatible);
  ASSERT_TRUE(plan.report.ok);
  ASSERT_TRUE(plan.node_plans.at("node_0").node.config.empty());
  EXPECT_EQ(plan.node_plans.at("node_0").normalized_config.at("retry_limit"),
            3);

  PlanTestNode::observed_config = nlohmann::json::object();
  Pipeline pipeline;
  PipelineDiagnostic diagnostic;
  ASSERT_TRUE(
      pipeline.BuildFromJson(pipeline_json, &diagnostic,
                             ValidationPolicy::kPrivateExtensionCompatible))
      << diagnostic.message;
  EXPECT_EQ(PlanTestNode::observed_config.at("retry_limit"), 3);
  EXPECT_EQ(pipeline.GetBizName(), "unregistered_test_biz");
  EXPECT_EQ(pipeline.GetTopologicalOrder(),
            std::vector<std::string>({"node_0"}));
}

TEST(ValidatedPipelinePlanTest, MultiLayerWavefrontTopology) {
  // Test DAG Wavefront layers calculation
  nlohmann::json dag_json = {
      {"biz_name", "unregistered_test_biz"},
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

  auto plan = PipelineValidator::ValidateAndPlan(
      dag_json, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_TRUE(plan.report.ok);
  ASSERT_EQ(plan.topological_layers.size(), 2u);
  // Layer 0 has node_a and node_b
  EXPECT_EQ(plan.topological_layers[0].size(), 2u);
  // Layer 1 has node_c
  EXPECT_EQ(plan.topological_layers[1].size(), 1u);
  EXPECT_EQ(plan.topological_layers[1][0], "node_c");
}

TEST(ValidatedPipelinePlanTest, RejectsSharedSerializedModelInParallelLayer) {
  nlohmann::json pipeline_json = {
      {"biz_name", "unregistered_test_biz"},
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

  auto plan = PipelineValidator::ValidateAndPlan(
      pipeline_json, ValidationPolicy::kPrivateExtensionCompatible);
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

TEST(ValidatedPipelinePlanTest, RejectsNodeFromDifferentBusiness) {
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

TEST(ValidatedPipelinePlanTest,
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

  // Layer 2 only performs deterministic lexical normalization. Deployment
  // roots are a Layer 1 concern.
  auto plan = PipelineValidator::ValidateAndPlan(
      pipeline_json, ValidationPolicy::kPrivateExtensionCompatible);
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump();
  EXPECT_EQ(plan.models[0].resolved_model_path, "models/sub/model.onnx");
  EXPECT_EQ(plan.models[1].resolved_model_path, "/opt/models/fixed.onnx");
  EXPECT_EQ(plan.models[2].resolved_model_path, "model_direct.onnx");

  auto plan_repeat = PipelineValidator::ValidateAndPlan(
      pipeline_json, ValidationPolicy::kPrivateExtensionCompatible);
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
  auto plan_escape = PipelineValidator::ValidateAndPlan(
      escape_json, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_FALSE(plan_escape.report.ok);
}

}  // namespace llm_edgeflow

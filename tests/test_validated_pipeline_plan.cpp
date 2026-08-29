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
#include "engine/engine_interface.h"
#include "engine/engine_registry.h"
#include "engine/model_registry.h"

namespace alg_framework {

class PlanTestNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "PlanTestNode";
  bool Init(const nlohmann::json&, SessionContext*) override { return true; }
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
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(PlanTestNode, MakePlanTestNodeDefinition());

class SerializedPlanTestEngine : public IModelEngine {
 public:
  inline static constexpr char kEngineType[] = "serialized_plan_test";

  bool Load(const std::string&, const nlohmann::json&) override { return true; }
  size_t GetMaxBatchSize() const override { return 1; }
  const std::string& EngineType() const override {
    static const std::string type = kEngineType;
    return type;
  }
};

EngineDefinition MakeSerializedPlanTestEngineDefinition() {
  EngineDefinition def;
  def.engine_type = SerializedPlanTestEngine::kEngineType;
  def.capability = "plan_test";
  def.description = "Serialized engine used by plan validation tests";
  def.thread_model = EngineThreadModel::kSerialized;
  return def;
}

REGISTER_ENGINE_WITH_DEFINITION(SerializedPlanTestEngine,
                                MakeSerializedPlanTestEngineDefinition());

class ModelBoundPlanTestNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "ModelBoundPlanTestNode";
  bool Init(const nlohmann::json&, SessionContext*) override { return true; }
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
  bool Init(const nlohmann::json&, SessionContext*) override { return true; }
  int Process(AlgContext*) override { return 0; }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};

class FlowContractConsumerNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "FlowContractConsumerNode";
  bool Init(const nlohmann::json&, SessionContext*) override { return true; }
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
  def.outputs = {PortDefinition{"flow", "TextBatch", true, false, "1:N",
                                "generate_sub_id", "request"}};
  return def;
}

NodeDefinition MakeFlowContractConsumerDefinition() {
  NodeDefinition def;
  def.node_type = FlowContractConsumerNode::kNodeType;
  def.category = "test";
  def.description = "Requires incompatible flow metadata";
  def.inputs = {PortDefinition{"flow", "TextBatch", true, false, "1:1",
                               "independent", "session"}};
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
      {DiagnosticCode::kUnknownBusiness, "UNKNOWN_BUSINESS"},
      {DiagnosticCode::kUnknownNodeType, "UNKNOWN_NODE_TYPE"},
      {DiagnosticCode::kUnknownEngineType, "UNKNOWN_ENGINE_TYPE"},
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
      {DiagnosticCode::kNodeBusinessMismatch, "NODE_BUSINESS_MISMATCH"},
      {DiagnosticCode::kMissingInputProducer, "MISSING_INPUT_PRODUCER"},
      {DiagnosticCode::kDuplicatePortProducer, "DUPLICATE_PORT_PRODUCER"},
      {DiagnosticCode::kMissingBusinessOutput, "MISSING_BUSINESS_OUTPUT"},
      {DiagnosticCode::kNodeNotParallelSafe, "NODE_NOT_PARALLEL_SAFE"},
      {DiagnosticCode::kParallelWriteConflict, "PARALLEL_WRITE_CONFLICT"},
      {DiagnosticCode::kSerializedEngineConcurrency,
       "SERIALIZED_ENGINE_CONCURRENCY"},
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
      {"business_name", "unregistered_test_biz"},
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

TEST(ValidatedPipelinePlanTest, ResolvesConfiguredPortLifetimeBeforePlanning) {
  nlohmann::json pipeline_json = {
      {"business_name", "smart_doc_qa_v1"},
      {"models",
       nlohmann::json::array({{{"model_id", "embed_model_v1"},
                               {"engine_type", "mock_npu_embedding"}}})},
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
      {"business_name", "unregistered_test_biz"},
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
            DiagnosticCode::kUnknownBusiness);

  auto compat_plan = PipelineValidator::ValidateAndPlan(
      unreg_biz_json, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_TRUE(compat_plan.report.ok);
  EXPECT_EQ(compat_plan.topological_order.size(), 1u);
  EXPECT_EQ(compat_plan.topological_layers.size(), 1u);
}

TEST(ValidatedPipelinePlanTest, MultiLayerWavefrontTopology) {
  // Test DAG Wavefront layers calculation
  nlohmann::json dag_json = {
      {"business_name", "unregistered_test_biz"},
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

TEST(ValidatedPipelinePlanTest, RejectsSharedSerializedEngineInParallelLayer) {
  nlohmann::json pipeline_json = {
      {"business_name", "unregistered_test_biz"},
      {"execution_mode", "parallel"},
      {"models",
       nlohmann::json::array(
           {{{"model_id", "shared"},
             {"engine_type", SerializedPlanTestEngine::kEngineType}}})},
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
        return item.code == DiagnosticCode::kSerializedEngineConcurrency;
      });
  ASSERT_NE(diagnostic, plan.report.diagnostics.end());
  EXPECT_EQ(diagnostic->node_id, "node_b");
  EXPECT_EQ(diagnostic->related_nodes, std::vector<std::string>({"node_a"}));
}

class RestrictedBusinessNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "RestrictedBusinessNode";
  bool Init(const nlohmann::json&, SessionContext*) override { return true; }
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
      {"business_name", "smart_doc_qa_v1"},
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array({{{"id", "wrong_business_node"},
                               {"node_type", "RestrictedBusinessNode"},
                               {"depends_on", nlohmann::json::array()}}})}};

  auto plan = PipelineValidator::ValidateAndPlan(pipeline_json);
  EXPECT_FALSE(plan.report.ok);
  EXPECT_NE(std::find_if(
                plan.report.diagnostics.begin(), plan.report.diagnostics.end(),
                [](const auto& item) {
                  return item.code == DiagnosticCode::kNodeBusinessMismatch;
                }),
            plan.report.diagnostics.end());
}

TEST(ValidatedPipelinePlanTest,
     DeterministicModelPathResolutionWithoutFilesystemProbing) {
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

  // 1. 无 model_root_dir
  auto plan_no_root = PipelineValidator::ValidateAndPlan(
      pipeline_json, ValidationPolicy::kPrivateExtensionCompatible, "");
  ASSERT_TRUE(plan_no_root.report.ok) << plan_no_root.report.ToJson().dump();
  EXPECT_EQ(plan_no_root.models[0].resolved_model_path,
            "models/sub/model.onnx");
  EXPECT_EQ(plan_no_root.models[1].resolved_model_path,
            "/opt/models/fixed.onnx");
  EXPECT_EQ(plan_no_root.models[2].resolved_model_path, "model_direct.onnx");

  // 2. 指定自定义 model_root_dir
  auto plan_with_root = PipelineValidator::ValidateAndPlan(
      pipeline_json, ValidationPolicy::kPrivateExtensionCompatible,
      "/custom/root");
  ASSERT_TRUE(plan_with_root.report.ok);
  EXPECT_EQ(plan_with_root.models[0].resolved_model_path,
            "/custom/root/models/sub/model.onnx");
  EXPECT_EQ(plan_with_root.models[1].resolved_model_path,
            "/opt/models/fixed.onnx");
  EXPECT_EQ(plan_with_root.models[2].resolved_model_path,
            "/custom/root/model_direct.onnx");

  // 3. 确定性保证：重复多次规划结果绝对一致
  auto plan_with_root_repeat = PipelineValidator::ValidateAndPlan(
      pipeline_json, ValidationPolicy::kPrivateExtensionCompatible,
      "/custom/root");
  ASSERT_TRUE(plan_with_root_repeat.report.ok);
  EXPECT_EQ(plan_with_root_repeat.models[0].resolved_model_path,
            plan_with_root.models[0].resolved_model_path);
  EXPECT_EQ(plan_with_root_repeat.models[1].resolved_model_path,
            plan_with_root.models[1].resolved_model_path);
  EXPECT_EQ(plan_with_root_repeat.models[2].resolved_model_path,
            plan_with_root.models[2].resolved_model_path);

  // 4. 路径逃逸检测
  nlohmann::json escape_json = pipeline_json;
  escape_json["models"][0]["model_path"] = "../escape.onnx";
  auto plan_escape = PipelineValidator::ValidateAndPlan(
      escape_json, ValidationPolicy::kPrivateExtensionCompatible,
      "/custom/root");
  EXPECT_FALSE(plan_escape.report.ok);
}

}  // namespace alg_framework

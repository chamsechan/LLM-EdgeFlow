#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <utility>
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
#include "tests/support/registry_test_access.h"

namespace llm_edgeflow {

class PlanTestNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "plan_test";
  inline static nlohmann::json observed_config = nlohmann::json::object();

  bool Init(const NodeInitContext& init_ctx) override {
    if (!init_ctx.plan) return false;
    observed_config = init_ctx.plan->normalized_params;
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
  inline static constexpr char kNodeType[] = "io_boundary_test";
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
  inline static constexpr char kImplName[] = "serialized_plan_test";

  static std::shared_ptr<IModel> Create(const ModelCreateContext&,
                                        std::string*) {
    return std::make_shared<SerializedPlanTestModel>();
  }
  const std::string& ImplName() const noexcept override {
    static const std::string type = kImplName;
    return type;
  }
  const std::string& ModelType() const noexcept override {
    static const std::string model_type = "plan_test";
    return model_type;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
};

class SerializedPlanSession : public IBackendSession {
 public:
  const std::string& BackendType() const noexcept override {
    static const std::string type = "serialized_plan_backend";
    return type;
  }
  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kFixture;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  BatchPolicy GetBatchPolicy() const noexcept override { return {16, 0}; }
};
class SerializedPlanBackend : public IInferenceBackend {
 public:
  const std::string& BackendType() const noexcept override {
    static const std::string type = "serialized_plan_backend";
    return type;
  }
  std::shared_ptr<IBackendSession> Load(const BackendLoadSpec&,
                                        std::string*) noexcept override {
    return std::make_shared<SerializedPlanSession>();
  }
};
BackendDefinition MakeSerializedPlanBackendDefinition() {
  BackendDefinition def;
  def.backend_type = "serialized_plan_backend";
  def.supported_protocols = {ExecutionProtocol::kFixture};
  def.concurrency = InferenceConcurrency::kConcurrent;
  return def;
}
REGISTER_BACKEND_WITH_DEFINITION(SerializedPlanBackend,
                                 MakeSerializedPlanBackendDefinition());

ModelDefinition MakeSerializedPlanTestModelDefinition() {
  ModelDefinition def;
  def.impl_name = SerializedPlanTestModel::kImplName;
  def.model_type = "plan_test";
  def.description = "Serialized model used by plan validation tests";
  def.required_protocol = ExecutionProtocol::kFixture;
  def.fixture_backends = {"serialized_plan_backend"};
  def.concurrency = InferenceConcurrency::kSerialized;
  return def;
}

REGISTER_MODEL_WITH_DEFINITION(SerializedPlanTestModel,
                               MakeSerializedPlanTestModelDefinition());

class ModelBoundPlanTestNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "model_bound_plan_test";
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
  inline static constexpr char kNodeType[] = "multi_model_plan_test";
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
  inline static constexpr char kNodeType[] = "flow_contract_producer";
  bool Init(const NodeInitContext&) override { return true; }
  int Process(AlgContext*) override { return 0; }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};

class FlowContractConsumerNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "flow_contract_consumer";
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

// 形状夹具：一个逐条配对、一个按请求分组、一个不透明关系。
// Validator 只关心它们的 Definition。
class ShapeFixtureNode : public INode {
 public:
  bool Init(const NodeInitContext&) override { return true; }
  int Process(AlgContext*) override { return 0; }
  const std::string& Name() const override {
    static const std::string name = "shape_fixture";
    return name;
  }
};

class ItemPairNode : public ShapeFixtureNode {
 public:
  inline static constexpr char kNodeType[] = "item_pair";
};

class RequestGroupNode : public ShapeFixtureNode {
 public:
  inline static constexpr char kNodeType[] = "request_group";
};

class OpaqueFlowNode : public ShapeFixtureNode {
 public:
  inline static constexpr char kNodeType[] = "opaque_flow";
};

NodeDefinition MakeShapeFixtureDefinition(std::string node_type,
                                          std::vector<NodePortDefinition> in,
                                          std::vector<NodePortDefinition> out) {
  NodeDefinition def;
  def.node_type = std::move(node_type);
  def.category = "test";
  def.description = "Port shape fixture";
  def.inputs = std::move(in);
  def.outputs = std::move(out);
  return def;
}

REGISTER_NODE_WITH_DEFINITION(
    ItemPairNode, MakeShapeFixtureDefinition(
                      ItemPairNode::kNodeType,
                      {NodePortDefinition{"left", "TextBatch", true},
                       NodePortDefinition{"right", "TextBatch", false}},
                      {NodePortDefinition{"paired", "TextBatch", true}}));
REGISTER_NODE_WITH_DEFINITION(
    RequestGroupNode,
    MakeShapeFixtureDefinition(
        RequestGroupNode::kNodeType,
        {NodePortDefinition{"items", "TextBatch", true, "N:1", "aggregate"}},
        {NodePortDefinition{"grouped", "TextBatch", true}}));
REGISTER_NODE_WITH_DEFINITION(
    OpaqueFlowNode,
    MakeShapeFixtureDefinition(
        OpaqueFlowNode::kNodeType,
        {NodePortDefinition{"items", "TextBatch", true, "N:M"}},
        {NodePortDefinition{"opaque", "TextBatch", true, "N:M"}}));

class ValidatedPipelinePlanTest : public ::testing::Test {};

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
      {DiagnosticCode::kDuplicateModelName, "DUPLICATE_MODEL_NAME"},
      {DiagnosticCode::kDuplicateNodeName, "DUPLICATE_NODE_NAME"},
      {DiagnosticCode::kInvalidNodeName, "INVALID_NODE_NAME"},
      {DiagnosticCode::kUnknownNodeReference, "UNKNOWN_NODE_REFERENCE"},
      {DiagnosticCode::kUnknownPortReference, "UNKNOWN_PORT_REFERENCE"},
      {DiagnosticCode::kPortTypeMismatch, "PORT_TYPE_MISMATCH"},
      {DiagnosticCode::kUnknownNodeType, "UNKNOWN_NODE_TYPE"},
      {DiagnosticCode::kUnknownModelType, "UNKNOWN_MODEL_TYPE"},
      {DiagnosticCode::kUnknownBackend, "UNKNOWN_BACKEND"},
      {DiagnosticCode::kModelTypeMismatch, "MODEL_TYPE_MISMATCH"},
      {DiagnosticCode::kBackendProtocolMismatch, "BACKEND_PROTOCOL_MISMATCH"},
      {DiagnosticCode::kInvalidDependency, "INVALID_DEPENDENCY"},
      {DiagnosticCode::kDuplicateDependency, "DUPLICATE_DEPENDENCY"},
      {DiagnosticCode::kDagCycle, "DAG_CYCLE"},
      {DiagnosticCode::kRegistryConflict, "REGISTRY_CONFLICT"},
      {DiagnosticCode::kUnknownConfigField, "UNKNOWN_CONFIG_FIELD"},
      {DiagnosticCode::kMissingConfigField, "MISSING_CONFIG_FIELD"},
      {DiagnosticCode::kConfigFieldType, "CONFIG_FIELD_TYPE"},
      {DiagnosticCode::kConfigFieldRange, "CONFIG_FIELD_RANGE"},
      {DiagnosticCode::kConfigFieldEnum, "CONFIG_FIELD_ENUM"},
      {DiagnosticCode::kUnusedModel, "UNUSED_MODEL"},
      {DiagnosticCode::kUnknownModelReference, "UNKNOWN_MODEL_REFERENCE"},
      {DiagnosticCode::kMissingInputProducer, "MISSING_INPUT_PRODUCER"},
      {DiagnosticCode::kDuplicatePortProducer, "DUPLICATE_PORT_PRODUCER"},
      {DiagnosticCode::kMissingOutputProducer, "MISSING_OUTPUT_PRODUCER"},
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

  EXPECT_EQ(cases.size(), 42u);
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
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array(
           {{{"name", "producer"},
             {"type", FlowContractProducerNode::kNodeType},
             {"depends_on", nlohmann::json::array()}},
            {{"name", "consumer"},
             {"type", FlowContractConsumerNode::kNodeType},
             {"inputs", {{"flow", "producer.flow"}}},
             {"depends_on", nlohmann::json::array({"producer"})}}})}};

  auto plan =
      PipelineValidator::ValidateAndPlan(pipeline_json, MakeTestBoundary());
  EXPECT_FALSE(plan.report.ok);
  const std::unordered_set<DiagnosticCode> expected = {
      DiagnosticCode::kPortProvenanceMismatch,
      DiagnosticCode::kPortLifetimeMismatch};
  std::unordered_set<DiagnosticCode> actual;
  for (const auto& diagnostic : plan.report.diagnostics) {
    actual.insert(diagnostic.code);
    if (expected.count(diagnostic.code)) {
      EXPECT_EQ(diagnostic.path, "/pipeline/1/inputs/flow");
      EXPECT_EQ(diagnostic.node_name, "consumer");
      EXPECT_EQ(diagnostic.port, "flow");
      EXPECT_EQ(diagnostic.related_nodes,
                std::vector<std::string>({"producer"}));
    }
  }
  for (const auto code : expected) EXPECT_TRUE(actual.count(code));
  // 逐条输入接受拆分后的条目；只有 provenance 和 lifetime 校验失败。
  EXPECT_FALSE(actual.count(DiagnosticCode::kPortCardinalityMismatch));
}

class PortShapeTest : public ::testing::Test {
 protected:
  static nlohmann::json Node(const std::string& name, const std::string& type,
                             nlohmann::json inputs) {
    return {{"name", name}, {"type", type}, {"inputs", std::move(inputs)}};
  }

  static nlohmann::json Split(const std::string& name) {
    return Node(name, FlowContractProducerNode::kNodeType,
                nlohmann::json::object());
  }

  ValidationReport Validate(nlohmann::json nodes,
                            std::vector<IoPortDefinition> outputs = {}) const {
    for (auto& output : outputs) {
      output.logical_name =
          output.blackboard_key.substr(output.blackboard_key.find('.') + 1);
      output.path = "/io/output/0/inputs";
    }
    return PipelineValidator::Validate(
        {{"pipeline", std::move(nodes)}},
        MakeTestBoundary({{"input.request", "TextBatch", true}},
                         std::move(outputs)));
  }

  static std::vector<ValidationDiagnostic> CardinalityErrors(
      const ValidationReport& report) {
    std::vector<ValidationDiagnostic> found;
    for (const auto& diagnostic : report.diagnostics) {
      if (diagnostic.code == DiagnosticCode::kPortCardinalityMismatch)
        found.push_back(diagnostic);
    }
    return found;
  }
};

TEST_F(ValidatedPipelinePlanTest, SerializesFactsOnlyWhenPresent) {
  ValidationDiagnostic diagnostic;
  EXPECT_TRUE(diagnostic.facts.is_object());
  EXPECT_TRUE(diagnostic.facts.empty());
  EXPECT_FALSE(diagnostic.ToJson().contains("facts"));
  EXPECT_FALSE(diagnostic.ToJson().contains("remediation"));

  diagnostic.facts = {{"actual_shape", {{"kind", "unknown"}}}};
  EXPECT_EQ(diagnostic.ToJson().at("facts"), diagnostic.facts);
  EXPECT_FALSE(diagnostic.ToJson().contains("remediation"));
}

TEST_F(PortShapeTest, ItemWiseNodeAcceptsSplitItems) {
  const auto report =
      Validate({Split("split"), Node("pair", ItemPairNode::kNodeType,
                                     {{"left", "split.flow"}})});
  EXPECT_TRUE(report.ok) << report.ToJson().dump();
}

TEST_F(PortShapeTest, SplitItemsCannotReachOnePerRequestEgress) {
  const auto report = Validate(
      {Split("split"),
       Node("pair", ItemPairNode::kNodeType, {{"left", "split.flow"}})},
      {{"pair.paired", "TextBatch", true, "1:1", "preserve"}});
  const auto errors = CardinalityErrors(report);
  ASSERT_EQ(errors.size(), 1u) << report.ToJson().dump();
  EXPECT_EQ(errors[0].path, "/io/output/0/inputs/paired");
  EXPECT_EQ(errors[0].node_name, "pair");
  EXPECT_EQ(errors[0].port, "paired");
  EXPECT_EQ(errors[0].related_nodes, std::vector<std::string>({"output"}));
  EXPECT_NE(errors[0].message.find("'split.flow'"), std::string::npos);
  EXPECT_FALSE(errors[0].remediation.has_value());
  const nlohmann::json declaration = {{"type_id", "TextBatch"},
                                      {"cardinality", "1:1"},
                                      {"provenance_policy", "preserve"},
                                      {"lifetime", "request"}};
  EXPECT_EQ(errors[0].facts.at("producer_name"), "pair");
  EXPECT_EQ(errors[0].facts.at("consumer_name"), "output");
  EXPECT_EQ(errors[0].facts.at("bound_key"), "pair.paired");
  EXPECT_EQ(errors[0].facts.at("actual"), declaration);
  EXPECT_EQ(errors[0].facts.at("expected"), declaration);
  EXPECT_EQ(errors[0].facts.at("actual_shape"),
            (nlohmann::json{{"kind", "multi"}, {"origin", "split.flow"}}));
  EXPECT_EQ(errors[0].facts.at("expected_shape"),
            (nlohmann::json{{"kind", "per_request"}}));
  EXPECT_EQ(errors[0].ToJson().at("facts"), errors[0].facts);
  EXPECT_FALSE(errors[0].ToJson().contains("remediation"));
}

TEST_F(PortShapeTest, CollectionEgressAcceptsSplitItems) {
  const auto report = Validate(
      {Split("split"),
       Node("pair", ItemPairNode::kNodeType, {{"left", "split.flow"}})},
      {{"pair.paired", "TextBatch", true, "N:1", "aggregate"}});
  EXPECT_TRUE(report.ok) << report.ToJson().dump();
}

TEST_F(PortShapeTest, AggregatedItemsTransformItemWiseToOnePerRequest) {
  const auto report = Validate(
      {Split("split"),
       Node("group", RequestGroupNode::kNodeType, {{"items", "split.flow"}}),
       Node("pair", ItemPairNode::kNodeType,
            {{"left", "group.grouped"}, {"right", "input.request"}})},
      {{"pair.paired", "TextBatch", true, "1:1", "preserve"}});
  EXPECT_TRUE(report.ok) << report.ToJson().dump();
}

TEST_F(PortShapeTest, MisalignedItemWiseInputsAreRejected) {
  const auto report =
      Validate({Split("split"),
                Node("pair", ItemPairNode::kNodeType,
                     {{"left", "input.request"}, {"right", "split.flow"}})});
  const auto errors = CardinalityErrors(report);
  ASSERT_EQ(errors.size(), 1u) << report.ToJson().dump();
  EXPECT_EQ(errors[0].path, "/pipeline/1/inputs/right");
  EXPECT_EQ(errors[0].node_name, "pair");
  EXPECT_EQ(errors[0].port, "right");
  EXPECT_EQ(errors[0].related_nodes, std::vector<std::string>({"split"}));
  EXPECT_FALSE(errors[0].remediation.has_value());
  EXPECT_EQ(errors[0].facts.at("producer_name"), "split");
  EXPECT_EQ(errors[0].facts.at("consumer_name"), "pair");
  EXPECT_EQ(errors[0].facts.at("bound_key"), "split.flow");
  EXPECT_EQ(errors[0].facts.at("anchor_port"), "left");
  EXPECT_EQ(errors[0].facts.at("anchor_key"), "input.request");
  EXPECT_EQ(errors[0].facts.at("actual").at("cardinality"), "1:N");
  EXPECT_EQ(errors[0].facts.at("expected").at("cardinality"), "1:1");
  EXPECT_EQ(errors[0].facts.at("actual_shape"),
            (nlohmann::json{{"kind", "multi"}, {"origin", "split.flow"}}));
  EXPECT_EQ(errors[0].facts.at("expected_shape"),
            (nlohmann::json{{"kind", "per_request"}}));
}

TEST_F(PortShapeTest, ItemWiseInputsPairOnlyWithinOneFanOut) {
  const auto same_origin = Validate(
      {Split("split"),
       Node("first", ItemPairNode::kNodeType, {{"left", "split.flow"}}),
       Node("second", ItemPairNode::kNodeType, {{"left", "split.flow"}}),
       Node("join", ItemPairNode::kNodeType,
            {{"left", "first.paired"}, {"right", "second.paired"}})});
  EXPECT_TRUE(same_origin.ok) << same_origin.ToJson().dump();

  const auto different_origins =
      Validate({Split("split_a"), Split("split_b"),
                Node("join", ItemPairNode::kNodeType,
                     {{"left", "split_a.flow"}, {"right", "split_b.flow"}})});
  const auto errors = CardinalityErrors(different_origins);
  ASSERT_EQ(errors.size(), 1u) << different_origins.ToJson().dump();
  EXPECT_EQ(errors[0].port, "right");
  EXPECT_FALSE(errors[0].remediation.has_value());
  EXPECT_EQ(errors[0].facts.at("producer_name"), "split_b");
  EXPECT_EQ(errors[0].facts.at("consumer_name"), "join");
  EXPECT_EQ(errors[0].facts.at("bound_key"), "split_b.flow");
  EXPECT_EQ(errors[0].facts.at("anchor_port"), "left");
  EXPECT_EQ(errors[0].facts.at("anchor_key"), "split_a.flow");
  EXPECT_EQ(errors[0].facts.at("actual_shape"),
            (nlohmann::json{{"kind", "multi"}, {"origin", "split_b.flow"}}));
  EXPECT_EQ(errors[0].facts.at("expected_shape"),
            (nlohmann::json{{"kind", "multi"}, {"origin", "split_a.flow"}}));
}

TEST_F(PortShapeTest, UnreservedConsumerNamesKeepNodeInputDiagnosticLocation) {
  for (const std::string id :
       {"$consumer", "$output_like", "PascalName", "自由名字"}) {
    SCOPED_TRACE(id);
    const auto shape_report =
        Validate({Split("split"),
                  Node(id, ItemPairNode::kNodeType,
                       {{"left", "input.request"}, {"right", "split.flow"}})});
    const auto shape_errors = CardinalityErrors(shape_report);
    ASSERT_EQ(shape_errors.size(), 1u) << shape_report.ToJson().dump();
    const auto& shape = shape_errors[0];
    EXPECT_EQ(shape.node_name, id);
    EXPECT_EQ(shape.related_nodes, std::vector<std::string>({"split"}));
    EXPECT_EQ(shape.path, "/pipeline/1/inputs/right");
    EXPECT_EQ(shape.port, "right");
    EXPECT_EQ(shape.facts.at("producer_name"), "split");
    EXPECT_EQ(shape.facts.at("consumer_name"), id);
    EXPECT_EQ(shape.facts.at("bound_key"), "split.flow");
    EXPECT_FALSE(shape.remediation.has_value());

    const auto flow_report =
        Validate({Split("split"), Node(id, FlowContractConsumerNode::kNodeType,
                                       {{"flow", "split.flow"}})});
    ASSERT_EQ(flow_report.diagnostics.size(), 2u)
        << flow_report.ToJson().dump();
    for (const auto& flow : flow_report.diagnostics) {
      EXPECT_TRUE(flow.code == DiagnosticCode::kPortProvenanceMismatch ||
                  flow.code == DiagnosticCode::kPortLifetimeMismatch);
      EXPECT_EQ(flow.node_name, id);
      EXPECT_EQ(flow.related_nodes, std::vector<std::string>({"split"}));
      EXPECT_EQ(flow.path, "/pipeline/1/inputs/flow");
      EXPECT_EQ(flow.port, "flow");
      EXPECT_EQ(flow.facts.at("producer_name"), "split");
      EXPECT_EQ(flow.facts.at("consumer_name"), id);
      EXPECT_EQ(flow.facts.at("bound_key"), "split.flow");
      EXPECT_FALSE(flow.remediation.has_value());
    }
  }
}

TEST_F(PortShapeTest, IngressPassthroughReportsNeutralIoOutputFacts) {
  PipelineIoBoundary boundary;
  boundary.input_published_ports = {IoPortDefinition{
      "input.request", "TextBatch", true, "1:N", "generate_sub_id"}};
  boundary.output_consumed_ports = {
      IoPortDefinition{"input.request", "TextBatch", true}};
  boundary.output_consumed_ports[0].logical_name = "request";
  boundary.output_consumed_ports[0].path = "/io/output/0/inputs";
  const auto report = PipelineValidator::Validate(
      {{"pipeline",
        {Node("probe", PlanTestNode::kNodeType, nlohmann::json::object())}}},
      boundary);
  const auto errors = CardinalityErrors(report);
  const auto output = std::find_if(
      errors.begin(), errors.end(),
      [](const auto& d) { return d.path == "/io/output/0/inputs/request"; });
  ASSERT_NE(output, errors.end()) << report.ToJson().dump();
  EXPECT_EQ(output->node_name, "input");
  EXPECT_EQ(output->related_nodes, std::vector<std::string>({"output"}));
  EXPECT_EQ(output->port, "request");
  EXPECT_EQ(output->facts.at("producer_name"), "input");
  EXPECT_EQ(output->facts.at("consumer_name"), "output");
  EXPECT_EQ(output->facts.at("bound_key"), "input.request");
  EXPECT_EQ(output->facts.at("actual").at("type_id"), "TextBatch");
  EXPECT_EQ(output->facts.at("expected").at("type_id"), "TextBatch");
  EXPECT_EQ(output->facts.at("actual_shape"),
            (nlohmann::json{{"kind", "multi"}, {"origin", "input.request"}}));
  EXPECT_EQ(output->facts.at("expected_shape"),
            (nlohmann::json{{"kind", "per_request"}}));
  EXPECT_FALSE(output->remediation.has_value());
}

TEST_F(PortShapeTest, UnknownShapeIsLeftToRuntimeChecks) {
  const auto report = Validate(
      {Split("split"),
       Node("opaque", OpaqueFlowNode::kNodeType, {{"items", "split.flow"}})},
      {{"opaque.opaque", "TextBatch", true, "1:1", "preserve"}});
  EXPECT_TRUE(report.ok) << report.ToJson().dump();
}

TEST_F(ValidatedPipelinePlanTest, ResolvesProducerLifetimeBeforePlanning) {
  const nlohmann::json document = {
      {"models",
       {{{"name", "embed_model"},
         {"type", "embedding"},
         {"backend", {{"type", "test_tensor_backend"}}},
         {"file", "fixture.bin"}}}},
      {"pipeline",
       {{{"name", "embed"},
         {"type", "text_embedding"},
         {"inputs", {{"text", "input.query_text"}}},
         {"params", {{"bind_model", "embed_model"}}}}}}};
  for (const std::string lifetime : {"request", "session"}) {
    SCOPED_TRACE(lifetime);
    auto boundary = MakeTestBoundary(
        {{"input.query_text", "TextBatch", true, "1:1", "preserve", lifetime}},
        {{"embed.embedding", "EmbeddingBatch", true}});
    const auto plan = PipelineValidator::ValidateAndPlan(document, boundary);
    ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump();
    const auto& node = plan.node_plans.at("embed");
    ASSERT_NE(node.FindPort("text"), nullptr);
    EXPECT_EQ(node.FindPort("text")->lifetime, lifetime);
    ASSERT_NE(node.FindPort("embedding", PortDirection::kOutput), nullptr);
    EXPECT_EQ(node.FindPort("embedding", PortDirection::kOutput)->lifetime,
              lifetime);
    EXPECT_FALSE(node.normalized_params.contains("cache"));
  }
}

TEST_F(ValidatedPipelinePlanTest, NormalizedNodeConfigIsRuntimeSingleSource) {
  nlohmann::json pipeline_json = {
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array({{{"name", "node_0"},
                               {"type", PlanTestNode::kNodeType},
                               {"depends_on", nlohmann::json::array()}}})}};

  auto plan =
      PipelineValidator::ValidateAndPlan(pipeline_json, MakeTestBoundary());
  ASSERT_TRUE(plan.report.ok);
  ASSERT_TRUE(plan.node_plans.at("node_0").node.params.empty());
  EXPECT_EQ(plan.node_plans.at("node_0").normalized_params.at("retry_limit"),
            3);

  PlanTestNode::observed_config = nlohmann::json::object();
  Pipeline pipeline;
  PipelineDiagnostic diagnostic;
  ASSERT_TRUE(BuildTestPipeline(pipeline, pipeline_json, MakeTestBoundary(),
                                &diagnostic))
      << diagnostic.message;
  EXPECT_EQ(PlanTestNode::observed_config.at("retry_limit"), 3);
  EXPECT_EQ(pipeline.GetTopologicalOrder(),
            std::vector<std::string>({"node_0"}));
}

TEST_F(ValidatedPipelinePlanTest, MultiLayerWavefrontTopology) {
  // 测试 DAG 波前分层计算
  nlohmann::json dag_json = {
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array({
           {{"name", "node_a"},
            {"type", "plan_test"},
            {"depends_on", nlohmann::json::array()}},
           {{"name", "node_b"},
            {"type", "plan_test"},
            {"depends_on", nlohmann::json::array()}},
           {{"name", "node_c"},
            {"type", "plan_test"},
            {"depends_on", nlohmann::json::array({"node_a", "node_b"})}},
       })}};

  auto plan = PipelineValidator::ValidateAndPlan(dag_json, MakeTestBoundary());
  EXPECT_TRUE(plan.report.ok);
  ASSERT_EQ(plan.report.topological_layers.size(), 2u);
  // 第 0 层为 node_a 和 node_b
  EXPECT_EQ(plan.report.topological_layers[0].size(), 2u);
  // 第 1 层为 node_c
  EXPECT_EQ(plan.report.topological_layers[1].size(), 1u);
  EXPECT_EQ(plan.report.topological_layers[1][0], "node_c");
}

TEST_F(ValidatedPipelinePlanTest,
       SplitsSharedSerializedModelIntoSequentialLayers) {
  nlohmann::json pipeline_json = {
      {"max_parallel_workers", 4},
      {"models",
       nlohmann::json::array({{{"name", "shared"},
                               {"type", "plan_test"},
                               {"backend",
                                {{"type", "serialized_plan_backend"},
                                 {"params", nlohmann::json::object()}}},
                               {"file", "serialized.bin"},
                               {"params", nlohmann::json::object()}}})},
      {"pipeline",
       nlohmann::json::array({{{"name", "node_a"},
                               {"type", ModelBoundPlanTestNode::kNodeType},
                               {"depends_on", nlohmann::json::array()},
                               {"params", {{"bind_model", "shared"}}}},
                              {{"name", "node_b"},
                               {"type", ModelBoundPlanTestNode::kNodeType},
                               {"depends_on", nlohmann::json::array()},
                               {"params", {{"bind_model", "shared"}}}}})}};

  auto plan =
      PipelineValidator::ValidateAndPlan(pipeline_json, MakeTestBoundary());
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
  EXPECT_EQ(plan.report.topological_layers,
            (std::vector<std::vector<std::string>>{{"node_a"}, {"node_b"}}));
  EXPECT_EQ(plan.report.topological_order,
            (std::vector<std::string>{"node_a", "node_b"}));
  pipeline_json["max_parallel_workers"] = 1;
  plan = PipelineValidator::ValidateAndPlan(pipeline_json, MakeTestBoundary());
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
  EXPECT_EQ(plan.report.topological_layers,
            (std::vector<std::vector<std::string>>{{"node_a", "node_b"}}));
  pipeline_json["max_parallel_workers"] = 4;
  pipeline_json["pipeline"][1]["depends_on"] = {"node_a"};
  EXPECT_TRUE(
      PipelineValidator::Validate(pipeline_json, MakeTestBoundary()).ok);
}

TEST_F(ValidatedPipelinePlanTest,
       ModelFilesRemainUnchangedWithoutDeploymentContext) {
  const auto boundary = MakeTestBoundary(
      {{"input.query_text", "TextBatch"},
       {"input.candidates", "RankedTextBatch", true, "N:1"}},
      {{"rank_relative.ranked", "RankedTextBatch", true, "N:1"}});
  if (!BackendRegistry::Instance().Find("mock_path_backend").has_value()) {
    BackendDefinition bdef;
    bdef.backend_type = "mock_path_backend";
    bdef.supported_protocols = {ExecutionProtocol::kFixture};
    bdef.concurrency = InferenceConcurrency::kConcurrent;
    BackendRegistry::Instance().Register(
        bdef, []() -> std::unique_ptr<IInferenceBackend> { return nullptr; });
  }

  if (!ModelRegistry::Instance().Find("mock_path_model").has_value()) {
    ModelDefinition mdef;
    mdef.impl_name = "mock_path_model";
    mdef.model_type = "rerank";
    mdef.required_protocol = ExecutionProtocol::kFixture;
    mdef.fixture_backends = {"mock_path_backend"};
    mdef.concurrency = InferenceConcurrency::kConcurrent;
    ModelRegistry::Instance().Register(
        mdef,
        [](const ModelCreateContext&, std::string*) -> std::shared_ptr<IModel> {
          return nullptr;
        });
  }

  nlohmann::json pipeline_json = {
      {"models",
       nlohmann::json::array({{{"name", "m_rel"},
                               {"type", "rerank"},
                               {"backend", {{"type", "mock_path_backend"}}},
                               {"file", "./models/sub/model.onnx"},
                               {"params", nlohmann::json::object()}},
                              {{"name", "m_abs"},
                               {"type", "rerank"},
                               {"backend", {{"type", "mock_path_backend"}}},
                               {"file", "/opt/models/fixed.onnx"},
                               {"params", nlohmann::json::object()}},
                              {{"name", "m_direct"},
                               {"type", "rerank"},
                               {"backend", {{"type", "mock_path_backend"}}},
                               {"file", "model_direct.onnx"},
                               {"params", nlohmann::json::object()}}})},
      {"pipeline",
       nlohmann::json::array({{{"name", "rank_relative"},
                               {"type", "text_rerank"},
                               {"depends_on", nlohmann::json::array()},
                               {"inputs",
                                {{"queries", "input.query_text"},
                                 {"candidates", "input.candidates"}}},
                               {"params", {{"bind_model", "m_rel"}}}}})}};

  for (const auto& [name, output] :
       {std::pair<const char*, const char*>{"m_abs", "ranked_abs"},
        {"m_direct", "ranked_direct"}}) {
    auto node = pipeline_json["pipeline"][0];
    node["name"] = std::string("rerank_") + name;
    node["params"]["bind_model"] = name;
    (void)output;
    pipeline_json["pipeline"].push_back(std::move(node));
  }

  // 流程编排层保留已给定的文件值，路径解析由接入适配层负责。
  auto plan = PipelineValidator::ValidateAndPlan(pipeline_json, boundary);
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump();
  EXPECT_EQ(plan.models[0].model_file, "./models/sub/model.onnx");
  EXPECT_EQ(plan.models[1].model_file, "/opt/models/fixed.onnx");
  EXPECT_EQ(plan.models[2].model_file, "model_direct.onnx");

  auto plan_repeat =
      PipelineValidator::ValidateAndPlan(pipeline_json, boundary);
  ASSERT_TRUE(plan_repeat.report.ok);
  EXPECT_EQ(plan_repeat.models[0].model_file, plan.models[0].model_file);
  EXPECT_EQ(plan_repeat.models[1].model_file, plan.models[1].model_file);
  EXPECT_EQ(plan_repeat.models[2].model_file, plan.models[2].model_file);
}

TEST_F(ValidatedPipelinePlanTest,
       RejectsIncompatibleEgressPortExecutionContracts) {
  auto boundary = MakeTestBoundary({}, {{"producer.flow", "TextBatch", true,
                                         "1:1", "independent", "session"}});
  boundary.output_consumed_ports[0].logical_name = "flow";
  boundary.output_consumed_ports[0].path = "/io/output/0/inputs";

  nlohmann::json pipeline_json = {
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array({{{"name", "producer"},
                               {"type", FlowContractProducerNode::kNodeType},
                               {"depends_on", nlohmann::json::array()}}})}};

  auto plan = PipelineValidator::ValidateAndPlan(pipeline_json, boundary);
  EXPECT_FALSE(plan.report.ok);

  bool found_cardinality = false;
  bool found_provenance = false;
  bool found_lifetime = false;
  for (const auto& diag : plan.report.diagnostics) {
    if (diag.code == DiagnosticCode::kPortCardinalityMismatch) {
      found_cardinality = true;
      EXPECT_EQ(diag.related_nodes, std::vector<std::string>({"output"}));
      EXPECT_EQ(diag.port, "flow");
    } else if (diag.code == DiagnosticCode::kPortProvenanceMismatch) {
      found_provenance = true;
      EXPECT_EQ(diag.related_nodes, std::vector<std::string>({"output"}));
      EXPECT_EQ(diag.port, "flow");
    } else if (diag.code == DiagnosticCode::kPortLifetimeMismatch) {
      found_lifetime = true;
      EXPECT_EQ(diag.related_nodes, std::vector<std::string>({"output"}));
      EXPECT_EQ(diag.port, "flow");
    }
  }
  EXPECT_TRUE(found_cardinality);
  EXPECT_TRUE(found_provenance);
  EXPECT_TRUE(found_lifetime);
}

TEST_F(ValidatedPipelinePlanTest,
       OptionalEgressMayBeAbsentButMustMatchWhenPresent) {
  auto boundary = MakeTestBoundary(
      {}, {{"", "Int32Batch", false, "N:M", "aggregate", "request"}});
  boundary.output_consumed_ports[0].logical_name = "optional";
  boundary.output_consumed_ports[0].path = "/io/output/0/inputs";
  const nlohmann::json config = {
      {"pipeline",
       {{{"name", "source"}, {"type", FlowContractProducerNode::kNodeType}}}}};
  EXPECT_TRUE(PipelineValidator::ValidateAndPlan(config, boundary).report.ok);
  boundary.output_consumed_ports[0].blackboard_key = "source.flow";
  boundary.output_consumed_ports[0].has_binding = true;
  const auto plan = PipelineValidator::ValidateAndPlan(config, boundary);
  EXPECT_FALSE(plan.report.ok);
  ASSERT_FALSE(plan.report.diagnostics.empty());
  EXPECT_EQ(plan.report.diagnostics.back().code,
            DiagnosticCode::kPortTypeMismatch);
  EXPECT_EQ(plan.report.diagnostics.back().path,
            "/io/output/0/inputs/optional");
  EXPECT_EQ(plan.report.diagnostics.back().related_nodes,
            std::vector<std::string>{"output"});
}

TEST_F(ValidatedPipelinePlanTest,
       MultiModelBindingsAndConcurrencyDeduplication) {
  nlohmann::json base_pipeline = {
      {"max_parallel_workers", 4},
      {"models", nlohmann::json::array({
                     {{"name", "shared_a"},
                      {"type", "plan_test"},
                      {"backend",
                       {{"type", "serialized_plan_backend"},
                        {"params", nlohmann::json::object()}}},
                      {"file", "model_a.bin"},
                      {"params", nlohmann::json::object()}},
                     {{"name", "independent_b"},
                      {"type", "plan_test"},
                      {"backend",
                       {{"type", "serialized_plan_backend"},
                        {"params", nlohmann::json::object()}}},
                      {"file", "model_b.bin"},
                      {"params", nlohmann::json::object()}},
                 })},
      {"pipeline",
       nlohmann::json::array({
           {{"name", "multi_node"},
            {"type", MultiModelPlanTestNode::kNodeType},
            {"depends_on", nlohmann::json::array()},
            {"params",
             {{"bind_generator", "shared_a"}, {"bind_reviewer", "shared_a"}}}},
           {{"name", "parallel_node"},
            {"type", ModelBoundPlanTestNode::kNodeType},
            {"depends_on", nlohmann::json::array()},
            {"params", {{"bind_model", "independent_b"}}}},
       })}};

  // 1. 同节点同实例去重且独立实例并行通过
  auto plan_ok =
      PipelineValidator::ValidateAndPlan(base_pipeline, MakeTestBoundary());
  ASSERT_TRUE(plan_ok.report.ok) << plan_ok.report.ToJson().dump(2);
  EXPECT_EQ(
      plan_ok.report.topological_layers,
      (std::vector<std::vector<std::string>>{{"multi_node", "parallel_node"}}));
  const auto& multi_plan = plan_ok.node_plans["multi_node"];
  ASSERT_EQ(multi_plan.model_bindings.size(), 2U);
  EXPECT_EQ(multi_plan.model_bindings[0].name, "generator");
  EXPECT_EQ(multi_plan.model_bindings[0].model_type, "plan_test");
  EXPECT_EQ(multi_plan.model_bindings[0].config_field, "bind_generator");
  EXPECT_EQ(multi_plan.model_bindings[0].model_name, "shared_a");

  EXPECT_EQ(multi_plan.model_bindings[1].name, "reviewer");
  EXPECT_EQ(multi_plan.model_bindings[1].model_type, "plan_test");
  EXPECT_EQ(multi_plan.model_bindings[1].config_field, "bind_reviewer");
  EXPECT_EQ(multi_plan.model_bindings[1].model_name, "shared_a");

  // 2. 其他节点共享 serialized 模型时拆到单独的层
  auto conflict_pipeline = base_pipeline;
  conflict_pipeline["pipeline"][1]["params"]["bind_model"] = "shared_a";
  conflict_pipeline["models"].erase(1);
  auto serialized_plan =
      PipelineValidator::ValidateAndPlan(conflict_pipeline, MakeTestBoundary());
  ASSERT_TRUE(serialized_plan.report.ok)
      << serialized_plan.report.ToJson().dump(2);
  EXPECT_EQ(serialized_plan.report.topological_layers,
            (std::vector<std::vector<std::string>>{{"multi_node"},
                                                   {"parallel_node"}}));
  EXPECT_EQ(serialized_plan.report.topological_order,
            (std::vector<std::string>{"multi_node", "parallel_node"}));
}

TEST_F(ValidatedPipelinePlanTest,
       IoBoundaryValidationCoversIngressEgressAndExtraWrites) {
  nlohmann::json valid_pipeline = {
      {"pipeline", nlohmann::json::array({
                       {{"name", "node1"},
                        {"type", "io_boundary_test"},
                        {"depends_on", nlohmann::json::array()},
                        {"inputs", {{"input_data", "input.text_in"}}}},
                   })}};

  // 1. 合法 IO boundary：覆盖必需 ingress，消费 egress
  PipelineIoBoundary valid_boundary;
  valid_boundary.input_published_ports = {
      IoPortDefinition("input.text_in", "TextBatch", true)};
  valid_boundary.output_consumed_ports = {
      IoPortDefinition("node1.output_data", "TextBatch", true)};

  auto plan_ok =
      PipelineValidator::ValidateAndPlan(valid_pipeline, valid_boundary);
  EXPECT_TRUE(plan_ok.report.ok);

  // 2. 缺失必需 ingress 端口发布
  PipelineIoBoundary missing_in_boundary;
  missing_in_boundary.output_consumed_ports =
      valid_boundary.output_consumed_ports;
  auto plan_missing_in =
      PipelineValidator::ValidateAndPlan(valid_pipeline, missing_in_boundary);
  EXPECT_FALSE(plan_missing_in.report.ok);
  auto diag_in =
      std::find_if(plan_missing_in.report.diagnostics.begin(),
                   plan_missing_in.report.diagnostics.end(), [](const auto& d) {
                     return d.code == DiagnosticCode::kUnknownPortReference;
                   });
  ASSERT_NE(diag_in, plan_missing_in.report.diagnostics.end());
  EXPECT_EQ(diag_in->path, "/pipeline/0/inputs/input_data");

  // 3. 缺失输出消费者所需的生产者
  PipelineIoBoundary missing_out_boundary = valid_boundary;
  missing_out_boundary.output_consumed_ports.push_back(
      IoPortDefinition("", "TextBatch", true));
  auto plan_missing_out =
      PipelineValidator::ValidateAndPlan(valid_pipeline, missing_out_boundary);
  EXPECT_FALSE(plan_missing_out.report.ok);
  auto diag_out = std::find_if(
      plan_missing_out.report.diagnostics.begin(),
      plan_missing_out.report.diagnostics.end(), [](const auto& d) {
        return d.code == DiagnosticCode::kMissingOutputProducer;
      });
  ASSERT_NE(diag_out, plan_missing_out.report.diagnostics.end());
  EXPECT_EQ(diag_out->path, "/io/output");

  // 4. 两个输入项发布同名逻辑端口。
  PipelineIoBoundary conflict_boundary = valid_boundary;
  conflict_boundary.input_published_ports.push_back(
      IoPortDefinition("input.text_in", "TextBatch", true));
  auto plan_conflict =
      PipelineValidator::ValidateAndPlan(valid_pipeline, conflict_boundary);
  EXPECT_FALSE(plan_conflict.report.ok);
  auto diag_conflict =
      std::find_if(plan_conflict.report.diagnostics.begin(),
                   plan_conflict.report.diagnostics.end(), [](const auto& d) {
                     return d.code == DiagnosticCode::kDuplicatePortProducer;
                   });
  ASSERT_NE(diag_conflict, plan_conflict.report.diagnostics.end());
}

TEST_F(ValidatedPipelinePlanTest, InfersDependenciesAndMergesExtraOrder) {
  const auto boundary = MakeTestBoundary({{"input.request", "TextBatch"}});
  nlohmann::json config = {
      {"pipeline",
       {{{"name", "consumer"},
         {"type", IoBoundaryTestNode::kNodeType},
         {"inputs", {{"input_data", "producer.output_data"}}},
         {"depends_on", {"barrier", "producer"}}},
        {{"name", "barrier"},
         {"type", PlanTestNode::kNodeType},
         {"depends_on", {"producer"}}},
        {{"name", "producer"},
         {"type", IoBoundaryTestNode::kNodeType},
         {"inputs", {{"input_data", "input.request"}}}}}}};
  const auto plan = PipelineValidator::ValidateAndPlan(config, boundary);
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
  EXPECT_EQ(plan.report.topological_order,
            (std::vector<std::string>{"producer", "barrier", "consumer"}));
  EXPECT_EQ(plan.report.topological_layers,
            (std::vector<std::vector<std::string>>{
                {"producer"}, {"barrier"}, {"consumer"}}));
  EXPECT_EQ(plan.config.max_parallel_workers, 1);
  const auto& dependencies = plan.node_plans.at("consumer").node.depends_on;
  EXPECT_EQ(
      std::unordered_set<std::string>(dependencies.begin(), dependencies.end()),
      (std::unordered_set<std::string>{"producer", "barrier"}));
  EXPECT_EQ(dependencies.size(), 2U);
  EXPECT_FALSE(config["pipeline"][2].contains("depends_on"));
}

TEST_F(ValidatedPipelinePlanTest, RejectsDataAndMixedDependencyCycles) {
  for (bool mixed : {false, true}) {
    SCOPED_TRACE(mixed ? "data plus order cycle" : "data cycle");
    nlohmann::json first = {{"name", "first"},
                            {"type", IoBoundaryTestNode::kNodeType},
                            {"inputs", {{"input_data", "second.output_data"}}}};
    nlohmann::json second = {{"name", "second"},
                             {"type", IoBoundaryTestNode::kNodeType},
                             {"inputs", {{"input_data", "first.output_data"}}}};
    if (mixed) {
      first["inputs"]["input_data"] = "input.request";
      first["depends_on"] = {"second"};
    }
    const auto boundary = MakeTestBoundary({{"input.request", "TextBatch"}});
    const auto plan = PipelineValidator::ValidateAndPlan(
        {{"pipeline", {first, second}}}, boundary);
    EXPECT_FALSE(plan.report.ok);
    EXPECT_TRUE(std::any_of(
        plan.report.diagnostics.begin(), plan.report.diagnostics.end(),
        [](const auto& d) { return d.code == DiagnosticCode::kDagCycle; }))
        << plan.report.ToJson().dump(2);
  }
}

TEST_F(ValidatedPipelinePlanTest, RejectsDataSelfCycle) {
  const auto plan = PipelineValidator::ValidateAndPlan(
      {{"pipeline",
        {{{"name", "self"},
          {"type", IoBoundaryTestNode::kNodeType},
          {"inputs", {{"input_data", "self.output_data"}}}}}}},
      MakeTestBoundary());
  EXPECT_FALSE(plan.report.ok);
  EXPECT_TRUE(std::any_of(
      plan.report.diagnostics.begin(), plan.report.diagnostics.end(),
      [](const auto& d) { return d.code == DiagnosticCode::kDagCycle; }))
      << plan.report.ToJson().dump(2);
}

TEST_F(ValidatedPipelinePlanTest,
       RequiredInputNeverFallsBackToSameNameProducer) {
  const auto plan = PipelineValidator::ValidateAndPlan(
      {{"pipeline",
        {{{"name", "producer"}, {"type", FlowContractProducerNode::kNodeType}},
         {{"name", "consumer"},
          {"type", FlowContractConsumerNode::kNodeType},
          {"depends_on", {"producer"}}}}}},
      MakeTestBoundary());
  EXPECT_FALSE(plan.report.ok);
  const auto diagnostic =
      std::find_if(plan.report.diagnostics.begin(),
                   plan.report.diagnostics.end(), [](const auto& d) {
                     return d.code == DiagnosticCode::kMissingInputProducer;
                   });
  ASSERT_NE(diagnostic, plan.report.diagnostics.end())
      << plan.report.ToJson().dump(2);
  EXPECT_EQ(diagnostic->path, "/pipeline/1/inputs");
  EXPECT_EQ(diagnostic->node_name, "consumer");
}

TEST_F(ValidatedPipelinePlanTest,
       ModelBindingIsRequiredEvenForOneMatchingModel) {
  const auto plan = PipelineValidator::ValidateAndPlan(
      {{"models",
        {{{"name", "shared"},
          {"type", "plan_test"},
          {"backend", {{"type", "serialized_plan_backend"}}},
          {"file", "fixture.bin"}}}},
       {"pipeline",
        {{{"name", "consumer"}, {"type", ModelBoundPlanTestNode::kNodeType}}}}},
      MakeTestBoundary());
  EXPECT_FALSE(plan.report.ok);
  EXPECT_TRUE(std::any_of(
      plan.report.diagnostics.begin(), plan.report.diagnostics.end(),
      [](const auto& d) {
        return d.code == DiagnosticCode::kMissingConfigField &&
               d.path == "/pipeline/0/params/bind_model";
      }))
      << plan.report.ToJson().dump(2);
}

TEST_F(ValidatedPipelinePlanTest,
       WorkerBudgetSchedulesUnsafeNodesSequentially) {
  nlohmann::json config = {
      {"pipeline",
       {{{"name", "source"}, {"type", FlowContractProducerNode::kNodeType}},
        {{"name", "independent"}, {"type", PlanTestNode::kNodeType}}}}};
  EXPECT_TRUE(PipelineValidator::Validate(config, MakeTestBoundary()).ok);
  config["max_parallel_workers"] = 1;
  const auto sequential_report =
      PipelineValidator::Validate(config, MakeTestBoundary());
  ASSERT_TRUE(sequential_report.ok) << sequential_report.ToJson().dump(2);
  EXPECT_EQ(sequential_report.topological_layers,
            (std::vector<std::vector<std::string>>{{"source", "independent"}}));
  config["max_parallel_workers"] = 2;
  const auto report = PipelineValidator::Validate(config, MakeTestBoundary());
  ASSERT_TRUE(report.ok) << report.ToJson().dump(2);
  EXPECT_EQ(report.topological_layers, (std::vector<std::vector<std::string>>{
                                           {"independent"}, {"source"}}));
  EXPECT_EQ(report.topological_order,
            (std::vector<std::string>{"independent", "source"}));
}

TEST_F(ValidatedPipelinePlanTest, PipelineBuildFromPlanLifecycle) {
  nlohmann::json valid_pipeline = {
      {"pipeline", nlohmann::json::array({
                       {{"name", "node1"},
                        {"type", "io_boundary_test"},
                        {"depends_on", nlohmann::json::array()},
                        {"inputs", {{"input_data", "input.text_in"}}}},
                   })}};

  PipelineIoBoundary boundary;
  boundary.input_published_ports = {
      IoPortDefinition("input.text_in", "TextBatch", true)};
  boundary.output_consumed_ports = {
      IoPortDefinition("node1.output_data", "TextBatch", true)};

  auto plan = std::make_unique<ValidatedPipelinePlan>(
      PipelineValidator::ValidateAndPlan(valid_pipeline, boundary));
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

TEST_F(ValidatedPipelinePlanTest, ExplicitSourcesHaveDistinctDiagnostics) {
  const auto boundary = MakeTestBoundary({{"input.text", "TextBatch"}});
  const nlohmann::json original = {
      {"pipeline",
       {{{"name", "consumer"},
         {"type", IoBoundaryTestNode::kNodeType},
         {"inputs", {{"input_data", "producer.output_data"}}}},
        {{"name", "producer"},
         {"type", IoBoundaryTestNode::kNodeType},
         {"inputs", {{"input_data", "input.text"}}}}}}};
  const auto valid = PipelineValidator::ValidateAndPlan(original, boundary);
  ASSERT_TRUE(valid.report.ok) << valid.report.ToJson().dump();
  EXPECT_EQ(valid.report.topological_order,
            (std::vector<std::string>{"producer", "consumer"}));
  EXPECT_EQ(
      valid.node_plans.at("consumer").FindPort("input_data")->blackboard_key,
      "producer.output_data");
  EXPECT_EQ(
      valid.node_plans.at("producer").FindPort("input_data")->blackboard_key,
      "input.text");
  for (const auto& [source, code] :
       std::vector<std::pair<std::string, DiagnosticCode>>{
           {"producerr.output_data", DiagnosticCode::kUnknownNodeReference},
           {"producer.missing", DiagnosticCode::kUnknownPortReference},
           {"input.missing", DiagnosticCode::kUnknownPortReference},
           {"", DiagnosticCode::kFieldType},
           {"bare_name", DiagnosticCode::kFieldType},
           {".output_data", DiagnosticCode::kFieldType},
           {"producer.", DiagnosticCode::kFieldType},
           {"producer.output.data", DiagnosticCode::kFieldType}}) {
    SCOPED_TRACE(source);
    auto document = original;
    document["pipeline"][0]["inputs"]["input_data"] = source;
    const auto plan = PipelineValidator::ValidateAndPlan(document, boundary);
    ASSERT_FALSE(plan.report.ok);
    const auto diagnostic = std::find_if(
        plan.report.diagnostics.begin(), plan.report.diagnostics.end(),
        [&](const auto& item) { return item.code == code; });
    ASSERT_NE(diagnostic, plan.report.diagnostics.end())
        << plan.report.ToJson();
    EXPECT_EQ(diagnostic->path, "/pipeline/0/inputs/input_data");
    EXPECT_EQ(diagnostic->node_name, "consumer");
    EXPECT_EQ(diagnostic->port, "input_data");
    if (code == DiagnosticCode::kUnknownNodeReference)
      EXPECT_NE(std::find(diagnostic->suggestions.begin(),
                          diagnostic->suggestions.end(), "producer"),
                diagnostic->suggestions.end());
    if (source == "producer.missing")
      EXPECT_EQ(diagnostic->suggestions,
                std::vector<std::string>{"output_data"});
  }
  auto wrong_type_boundary = boundary;
  wrong_type_boundary.input_published_ports[0].type_id = "Int32Batch";
  const auto wrong_type =
      PipelineValidator::ValidateAndPlan(original, wrong_type_boundary);
  const auto mismatch =
      std::find_if(wrong_type.report.diagnostics.begin(),
                   wrong_type.report.diagnostics.end(), [](const auto& item) {
                     return item.code == DiagnosticCode::kPortTypeMismatch;
                   });
  ASSERT_NE(mismatch, wrong_type.report.diagnostics.end());
  EXPECT_EQ(mismatch->path, "/pipeline/1/inputs/input_data");
}

TEST_F(ValidatedPipelinePlanTest, OutputSourcesHaveExactConverterLocations) {
  const nlohmann::json document = {
      {"pipeline",
       {{{"name", "producer"},
         {"type", IoBoundaryTestNode::kNodeType},
         {"inputs", {{"input_data", "input.text"}}}}}}};
  for (const auto& [source, code] :
       std::vector<std::pair<std::string, DiagnosticCode>>{
           {"missing.output_data", DiagnosticCode::kUnknownNodeReference},
           {"producer.missing", DiagnosticCode::kUnknownPortReference},
           {"", DiagnosticCode::kFieldType},
           {"producer.output.data", DiagnosticCode::kFieldType},
           {"producer.output_data", DiagnosticCode::kPortTypeMismatch}}) {
    auto boundary = MakeTestBoundary({{"input.text", "TextBatch"}},
                                     {{source, "Int32Batch"}});
    auto& output = boundary.output_consumed_ports[0];
    output.logical_name = "answer";
    output.path = "/io/output/3/inputs";
    output.has_binding = true;
    const auto plan = PipelineValidator::ValidateAndPlan(document, boundary);
    ASSERT_FALSE(plan.report.ok);
    const auto diagnostic = std::find_if(
        plan.report.diagnostics.begin(), plan.report.diagnostics.end(),
        [&](const auto& item) { return item.code == code; });
    ASSERT_NE(diagnostic, plan.report.diagnostics.end())
        << plan.report.ToJson();
    EXPECT_EQ(diagnostic->path, "/io/output/3/inputs/answer");
    EXPECT_EQ(diagnostic->port, "answer");
  }
  auto missing_boundary =
      MakeTestBoundary({{"input.text", "TextBatch"}}, {{"", "TextBatch"}});
  missing_boundary.output_consumed_ports[0].logical_name = "answer";
  missing_boundary.output_consumed_ports[0].path = "/io/output/3/inputs";
  const auto missing =
      PipelineValidator::ValidateAndPlan(document, missing_boundary);
  ASSERT_EQ(missing.report.diagnostics.size(), 1U) << missing.report.ToJson();
  EXPECT_EQ(missing.report.diagnostics[0].code,
            DiagnosticCode::kMissingOutputProducer);
  EXPECT_EQ(missing.report.diagnostics[0].path, "/io/output/3/inputs");
}

TEST_F(ValidatedPipelinePlanTest, ReservedAndDottedNamesFailBeforeBuild) {
  for (const std::string name : {"input", "output", "dot.name"}) {
    const auto plan = PipelineValidator::ValidateAndPlan(
        {{"pipeline", {{{"name", name}, {"type", PlanTestNode::kNodeType}}}}},
        MakeTestBoundary());
    ASSERT_FALSE(plan.report.ok);
    ASSERT_EQ(plan.report.diagnostics.size(), 1U);
    EXPECT_EQ(plan.report.diagnostics[0].code,
              DiagnosticCode::kInvalidNodeName);
    EXPECT_EQ(plan.report.diagnostics[0].path, "/pipeline/0/name");
    EXPECT_TRUE(plan.node_plans.empty());
  }
  const auto allowed = PipelineValidator::ValidateAndPlan(
      {{"pipeline",
        {{{"name", "Free-名字"}, {"type", PlanTestNode::kNodeType}}}}},
      MakeTestBoundary());
  EXPECT_TRUE(allowed.report.ok) << allowed.report.ToJson();
}

TEST_F(ValidatedPipelinePlanTest, PlansOnlyReferencedNativeOutputsAndIngress) {
  const nlohmann::json document = {{"pipeline",
                                    {{{"name", "chunk"},
                                      {"type", "text_chunk"},
                                      {"inputs", {{"text", "input.text"}}}}}}};
  const auto plan = PipelineValidator::ValidateAndPlan(
      document,
      MakeTestBoundary(
          {{"input.text", "TextBatch"}, {"input.unused", "TextBatch"}},
          {{"chunk.chunks", "TextBatch", true, "N:1", "aggregate"}}));
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson();
  ASSERT_EQ(plan.input_ports.size(), 1U);
  EXPECT_EQ(plan.input_ports[0].blackboard_key, "input.text");
  const auto& chunk = plan.node_plans.at("chunk");
  ASSERT_NE(chunk.FindPort("chunks", PortDirection::kOutput), nullptr);
  EXPECT_EQ(chunk.FindPort("chunks", PortDirection::kOutput)->blackboard_key,
            "chunk.chunks");
  EXPECT_EQ(chunk.FindPort("chunk_counts", PortDirection::kOutput), nullptr);
}

TEST_F(ValidatedPipelinePlanTest,
       FollowLifetimePropagatesAcrossDagAndPreparesFacts) {
  test_support::RegistryTestAccess::ScopedNodeState state;
  std::vector<std::string> prepared_lifetimes;
  NodeDefinition follow;
  follow.node_type = "follow_embedding_fixture";
  follow.inputs = {{"embedding", "EmbeddingBatch"}};
  follow.outputs = {{"embedding", "EmbeddingBatch", true, "1:1", "preserve",
                     "request", "embedding"}};
  follow.validate_config = [&](const nlohmann::json&, const BindingFacts& facts,
                               std::string*, std::string*) {
    prepared_lifetimes.push_back(facts.InputLifetime("embedding"));
    return facts.IsConnected("embedding");
  };
  ASSERT_TRUE(NodeRegistry::Instance().Register(
      follow.node_type, [] { return std::make_unique<ShapeFixtureNode>(); },
      follow));
  NodeDefinition consumer;
  consumer.node_type = "session_embedding_consumer_fixture";
  consumer.inputs = {
      {"embedding", "EmbeddingBatch", true, "N:1", "preserve", "session"}};
  ASSERT_TRUE(NodeRegistry::Instance().Register(
      consumer.node_type, [] { return std::make_unique<ShapeFixtureNode>(); },
      consumer));
  nlohmann::json document = {
      {"models",
       {{{"type", "embedding"},
         {"name", "encoder"},
         {"file", "fixture.bin"},
         {"backend", {{"type", "test_tensor_backend"}}}}}},
      {"pipeline",
       {{{"name", "consume"},
         {"type", consumer.node_type},
         {"inputs", {{"embedding", "follow.embedding"}}}},
        {{"name", "follow"},
         {"type", follow.node_type},
         {"inputs", {{"embedding", "embed.embedding"}}}},
        {{"name", "embed"},
         {"type", "text_embedding"},
         {"params", {{"bind_model", "encoder"}}},
         {"inputs", {{"text", "corpus.corpus"}}}},
        {{"name", "corpus"},
         {"type", "text_corpus_source"},
         {"params", {{"corpus", {"shared"}}}}}}}};
  const auto accepted =
      PipelineValidator::ValidateAndPlan(document, MakeTestBoundary());
  ASSERT_TRUE(accepted.report.ok) << accepted.report.ToJson();
  EXPECT_EQ(accepted.report.topological_order,
            (std::vector<std::string>{"corpus", "embed", "follow", "consume"}));
  EXPECT_EQ(prepared_lifetimes, std::vector<std::string>{"session"});
  EXPECT_EQ(accepted.node_plans.at("embed").FindPort("text")->lifetime,
            "session");
  EXPECT_EQ(accepted.node_plans.at("follow").FindPort("embedding")->lifetime,
            "session");
  EXPECT_EQ(accepted.node_plans.at("follow")
                .FindPort("embedding", PortDirection::kOutput)
                ->lifetime,
            "session");
  document["pipeline"][2]["inputs"]["text"] = "input.text";
  const auto rejected = PipelineValidator::ValidateAndPlan(
      document, MakeTestBoundary({{"input.text", "TextBatch"}}));
  ASSERT_FALSE(rejected.report.ok);
  EXPECT_EQ(prepared_lifetimes,
            (std::vector<std::string>{"session", "request"}));
  const auto diagnostic =
      std::find_if(rejected.report.diagnostics.begin(),
                   rejected.report.diagnostics.end(), [](const auto& item) {
                     return item.code == DiagnosticCode::kPortLifetimeMismatch;
                   });
  ASSERT_NE(diagnostic, rejected.report.diagnostics.end())
      << rejected.report.ToJson();
  EXPECT_EQ(diagnostic->path, "/pipeline/0/inputs/embedding");
  EXPECT_EQ(rejected.node_plans.at("follow")
                .FindPort("embedding", PortDirection::kOutput)
                ->lifetime,
            "request");
}

}  // namespace llm_edgeflow

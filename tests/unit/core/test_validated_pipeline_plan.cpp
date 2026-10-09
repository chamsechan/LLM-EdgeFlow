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

// 形状夹具：一个逐条配对、一个按请求分组、一个不透明关系。
// Validator 只关心它们的 Definition。
class ShapeFixtureNode : public INode {
 public:
  bool Init(const NodeInitContext&) override { return true; }
  int Process(AlgContext*) override { return 0; }
  const std::string& Name() const override {
    static const std::string name = "ShapeFixtureNode";
    return name;
  }
};

class ItemPairNode : public ShapeFixtureNode {
 public:
  inline static constexpr char kNodeType[] = "ItemPairNode";
};

class RequestGroupNode : public ShapeFixtureNode {
 public:
  inline static constexpr char kNodeType[] = "RequestGroupNode";
};

class OpaqueFlowNode : public ShapeFixtureNode {
 public:
  inline static constexpr char kNodeType[] = "OpaqueFlowNode";
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
      {DiagnosticCode::kDuplicateModelId, "DUPLICATE_MODEL_ID"},
      {DiagnosticCode::kDuplicateNodeId, "DUPLICATE_NODE_ID"},
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
      {DiagnosticCode::kMissingInputProducer, "MISSING_INPUT_PRODUCER"},
      {DiagnosticCode::kDuplicatePortProducer, "DUPLICATE_PORT_PRODUCER"},
      {DiagnosticCode::kMissingOutputProducer, "MISSING_OUTPUT_PRODUCER"},
      {DiagnosticCode::kParallelWriteConflict, "PARALLEL_WRITE_CONFLICT"},
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

  EXPECT_EQ(cases.size(), 40u);
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
           {{{"id", "producer"},
             {"node_type", FlowContractProducerNode::kNodeType},
             {"depends_on", nlohmann::json::array()}},
            {{"id", "consumer"},
             {"node_type", FlowContractConsumerNode::kNodeType},
             {"inputs", {{"flow", "flow"}}},
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
      EXPECT_EQ(diagnostic.node_id, "consumer");
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
  const PipelineIoBoundary boundary_ = MakeTestBoundary(
      {{"request", "TextBatch", true}},
      {{"result", "TextBatch", false, "1:1"},
       {"collection", "TextBatch", false, "N:1", "aggregate"}});

  static nlohmann::json Node(const std::string& id, const std::string& type,
                             nlohmann::json inputs, nlohmann::json outputs) {
    return {{"id", id},
            {"node_type", type},
            {"inputs", std::move(inputs)},
            {"outputs", std::move(outputs)}};
  }

  static nlohmann::json Split(const std::string& id, const std::string& key) {
    return Node(id, FlowContractProducerNode::kNodeType,
                nlohmann::json::object(), {{"flow", key}});
  }

  ValidationReport Validate(nlohmann::json nodes) const {
    return PipelineValidator::Validate({{"pipeline", std::move(nodes)}},
                                       boundary_);
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
      Validate({Split("split", "parts"),
                Node("pair", ItemPairNode::kNodeType, {{"left", "parts"}},
                     {{"paired", "paired_parts"}})});
  EXPECT_TRUE(report.ok) << report.ToJson().dump();
}

TEST_F(PortShapeTest, SplitItemsCannotReachOnePerRequestEgress) {
  const auto report =
      Validate({Split("split", "parts"),
                Node("pair", ItemPairNode::kNodeType, {{"left", "parts"}},
                     {{"paired", "result"}})});
  const auto errors = CardinalityErrors(report);
  ASSERT_EQ(errors.size(), 1u) << report.ToJson().dump();
  EXPECT_EQ(errors[0].path, "/io/output");
  EXPECT_EQ(errors[0].node_id, "pair");
  EXPECT_EQ(errors[0].port, "result");
  EXPECT_EQ(errors[0].related_nodes, std::vector<std::string>({"$io_output"}));
  EXPECT_NE(errors[0].message.find("'split.flow'"), std::string::npos);
  EXPECT_FALSE(errors[0].remediation.has_value());
  const nlohmann::json declaration = {{"type_id", "TextBatch"},
                                      {"cardinality", "1:1"},
                                      {"provenance_policy", "preserve"},
                                      {"lifetime", "request"}};
  EXPECT_EQ(errors[0].facts.at("producer_id"), "pair");
  EXPECT_EQ(errors[0].facts.at("consumer_id"), "$io_output");
  EXPECT_EQ(errors[0].facts.at("bound_key"), "result");
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
  const auto report =
      Validate({Split("split", "parts"),
                Node("pair", ItemPairNode::kNodeType, {{"left", "parts"}},
                     {{"paired", "collection"}})});
  EXPECT_TRUE(report.ok) << report.ToJson().dump();
}

TEST_F(PortShapeTest, AggregatedItemsTransformItemWiseToOnePerRequest) {
  const auto report =
      Validate({Split("split", "parts"),
                Node("group", RequestGroupNode::kNodeType, {{"items", "parts"}},
                     {{"grouped", "grouped"}}),
                Node("pair", ItemPairNode::kNodeType,
                     {{"left", "grouped"}, {"right", "request"}},
                     {{"paired", "result"}})});
  EXPECT_TRUE(report.ok) << report.ToJson().dump();
}

TEST_F(PortShapeTest, MisalignedItemWiseInputsAreRejected) {
  const auto report = Validate(
      {Split("split", "parts"), Node("pair", ItemPairNode::kNodeType,
                                     {{"left", "request"}, {"right", "parts"}},
                                     {{"paired", "paired_parts"}})});
  const auto errors = CardinalityErrors(report);
  ASSERT_EQ(errors.size(), 1u) << report.ToJson().dump();
  EXPECT_EQ(errors[0].path, "/pipeline/1/inputs/right");
  EXPECT_EQ(errors[0].node_id, "pair");
  EXPECT_EQ(errors[0].port, "right");
  EXPECT_EQ(errors[0].related_nodes, std::vector<std::string>({"split"}));
  EXPECT_FALSE(errors[0].remediation.has_value());
  EXPECT_EQ(errors[0].facts.at("producer_id"), "split");
  EXPECT_EQ(errors[0].facts.at("consumer_id"), "pair");
  EXPECT_EQ(errors[0].facts.at("bound_key"), "parts");
  EXPECT_EQ(errors[0].facts.at("anchor_port"), "left");
  EXPECT_EQ(errors[0].facts.at("anchor_key"), "request");
  EXPECT_EQ(errors[0].facts.at("actual").at("cardinality"), "1:N");
  EXPECT_EQ(errors[0].facts.at("expected").at("cardinality"), "1:1");
  EXPECT_EQ(errors[0].facts.at("actual_shape"),
            (nlohmann::json{{"kind", "multi"}, {"origin", "split.flow"}}));
  EXPECT_EQ(errors[0].facts.at("expected_shape"),
            (nlohmann::json{{"kind", "per_request"}}));
}

TEST_F(PortShapeTest, ItemWiseInputsPairOnlyWithinOneFanOut) {
  const auto same_origin =
      Validate({Split("split", "parts"),
                Node("first", ItemPairNode::kNodeType, {{"left", "parts"}},
                     {{"paired", "first_parts"}}),
                Node("second", ItemPairNode::kNodeType, {{"left", "parts"}},
                     {{"paired", "second_parts"}}),
                Node("join", ItemPairNode::kNodeType,
                     {{"left", "first_parts"}, {"right", "second_parts"}},
                     {{"paired", "joined"}})});
  EXPECT_TRUE(same_origin.ok) << same_origin.ToJson().dump();

  const auto different_origins =
      Validate({Split("split_a", "parts_a"), Split("split_b", "parts_b"),
                Node("join", ItemPairNode::kNodeType,
                     {{"left", "parts_a"}, {"right", "parts_b"}},
                     {{"paired", "joined"}})});
  const auto errors = CardinalityErrors(different_origins);
  ASSERT_EQ(errors.size(), 1u) << different_origins.ToJson().dump();
  EXPECT_EQ(errors[0].port, "right");
  EXPECT_FALSE(errors[0].remediation.has_value());
  EXPECT_EQ(errors[0].facts.at("producer_id"), "split_b");
  EXPECT_EQ(errors[0].facts.at("consumer_id"), "join");
  EXPECT_EQ(errors[0].facts.at("bound_key"), "parts_b");
  EXPECT_EQ(errors[0].facts.at("anchor_port"), "left");
  EXPECT_EQ(errors[0].facts.at("anchor_key"), "parts_a");
  EXPECT_EQ(errors[0].facts.at("actual_shape"),
            (nlohmann::json{{"kind", "multi"}, {"origin", "split_b.flow"}}));
  EXPECT_EQ(errors[0].facts.at("expected_shape"),
            (nlohmann::json{{"kind", "multi"}, {"origin", "split_a.flow"}}));
}

TEST_F(PortShapeTest,
       DollarPrefixedConsumerIdsKeepNodeInputDiagnosticLocation) {
  for (const std::string id :
       {"$consumer", "$egress", "$io_output", "$ingress"}) {
    SCOPED_TRACE(id);
    const auto shape_report =
        Validate({Split("split", "parts"),
                  Node(id, ItemPairNode::kNodeType,
                       {{"left", "request"}, {"right", "parts"}},
                       {{"paired", "paired_parts"}})});
    const auto shape_errors = CardinalityErrors(shape_report);
    ASSERT_EQ(shape_errors.size(), 1u) << shape_report.ToJson().dump();
    const auto& shape = shape_errors[0];
    EXPECT_EQ(shape.node_id, id);
    EXPECT_EQ(shape.related_nodes, std::vector<std::string>({"split"}));
    EXPECT_EQ(shape.path, "/pipeline/1/inputs/right");
    EXPECT_EQ(shape.port, "right");
    EXPECT_EQ(shape.facts.at("producer_id"), "split");
    EXPECT_EQ(shape.facts.at("consumer_id"), id);
    EXPECT_EQ(shape.facts.at("bound_key"), "parts");
    EXPECT_FALSE(shape.remediation.has_value());

    const auto flow_report =
        Validate({Split("split", "parts"),
                  Node(id, FlowContractConsumerNode::kNodeType,
                       {{"flow", "parts"}}, nlohmann::json::object())});
    ASSERT_EQ(flow_report.diagnostics.size(), 2u)
        << flow_report.ToJson().dump();
    for (const auto& flow : flow_report.diagnostics) {
      EXPECT_TRUE(flow.code == DiagnosticCode::kPortProvenanceMismatch ||
                  flow.code == DiagnosticCode::kPortLifetimeMismatch);
      EXPECT_EQ(flow.node_id, id);
      EXPECT_EQ(flow.related_nodes, std::vector<std::string>({"split"}));
      EXPECT_EQ(flow.path, "/pipeline/1/inputs/flow");
      EXPECT_EQ(flow.port, "flow");
      EXPECT_EQ(flow.facts.at("producer_id"), "split");
      EXPECT_EQ(flow.facts.at("consumer_id"), id);
      EXPECT_EQ(flow.facts.at("bound_key"), "parts");
      EXPECT_FALSE(flow.remediation.has_value());
    }
  }
}

TEST_F(PortShapeTest, IngressPassthroughReportsNeutralIoOutputFacts) {
  PipelineIoBoundary boundary;
  boundary.input_published_ports = {
      IoPortDefinition{"request", "TextBatch", true, "1:N", "generate_sub_id"}};
  boundary.output_consumed_ports = {
      IoPortDefinition{"request", "TextBatch", true}};
  const auto report = PipelineValidator::Validate(
      {{"pipeline",
        {Node("probe", PlanTestNode::kNodeType, nlohmann::json::object(),
              nlohmann::json::object())}}},
      boundary);
  const auto errors = CardinalityErrors(report);
  const auto output =
      std::find_if(errors.begin(), errors.end(),
                   [](const auto& d) { return d.path == "/io/output"; });
  ASSERT_NE(output, errors.end()) << report.ToJson().dump();
  EXPECT_EQ(output->node_id, "$ingress");
  EXPECT_EQ(output->related_nodes, std::vector<std::string>({"$io_output"}));
  EXPECT_EQ(output->port, "request");
  EXPECT_EQ(output->facts.at("producer_id"), "$ingress");
  EXPECT_EQ(output->facts.at("consumer_id"), "$io_output");
  EXPECT_EQ(output->facts.at("bound_key"), "request");
  EXPECT_EQ(output->facts.at("actual").at("type_id"), "TextBatch");
  EXPECT_EQ(output->facts.at("expected").at("type_id"), "TextBatch");
  EXPECT_EQ(
      output->facts.at("actual_shape"),
      (nlohmann::json{{"kind", "multi"}, {"origin", "$ingress.request"}}));
  EXPECT_EQ(output->facts.at("expected_shape"),
            (nlohmann::json{{"kind", "per_request"}}));
  EXPECT_FALSE(output->remediation.has_value());
}

TEST_F(PortShapeTest, UnknownShapeIsLeftToRuntimeChecks) {
  const auto report =
      Validate({Split("split", "parts"),
                Node("opaque", OpaqueFlowNode::kNodeType, {{"items", "parts"}},
                     {{"opaque", "result"}})});
  EXPECT_TRUE(report.ok) << report.ToJson().dump();
}

TEST_F(ValidatedPipelinePlanTest,
       RejectsDuplicateProducerEvenWhenDefinitionAllowsOverride) {
  nlohmann::json pipeline_json = {
      {"models", nlohmann::json::array()},
      {"pipeline", nlohmann::json::array(
                       {{{"id", "first"},
                         {"node_type", FlowContractProducerNode::kNodeType},
                         {"depends_on", nlohmann::json::array()}},
                        {{"id", "second"},
                         {"node_type", FlowContractProducerNode::kNodeType},
                         {"depends_on", nlohmann::json::array({"first"})}}})}};

  const auto plan =
      PipelineValidator::ValidateAndPlan(pipeline_json, MakeTestBoundary());
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

TEST_F(ValidatedPipelinePlanTest, RejectsNodeOutputBoundToIoIngress) {
  const auto boundary = MakeTestBoundary(
      {{"raw_docs", "TextBatch"}, {"raw_queries", "TextBatch"}},
      {{"llm_answers", "TextBatch"},
       {"intent_matches", "RuleMatchBatch"},
       {"doc_chunk_counts", "Int32Batch"}});
  std::ifstream stream("configs/pipeline_doc_qa_default.json");
  ASSERT_TRUE(stream.is_open());
  nlohmann::json pipeline_json;
  stream >> pipeline_json;
  pipeline_json.erase("io");
  const size_t source_index = pipeline_json["pipeline"].size();
  pipeline_json["pipeline"].push_back(
      {{"id", "ingress_collision"},
       {"node_type", "TextChunkNode"},
       {"depends_on", nlohmann::json::array()},
       {"inputs", {{"text", "raw_docs"}}},
       {"outputs", {{"chunks", "raw_docs"}, {"chunk_counts", "audit_counts"}}},
       {"config", {{"chunk_size", 60}}}});

  const auto plan = PipelineValidator::ValidateAndPlan(pipeline_json, boundary);
  ASSERT_FALSE(plan.report.ok);
  const auto diagnostic =
      std::find_if(plan.report.diagnostics.begin(),
                   plan.report.diagnostics.end(), [](const auto& item) {
                     return item.code == DiagnosticCode::kDuplicatePortProducer;
                   });
  ASSERT_NE(diagnostic, plan.report.diagnostics.end());
  EXPECT_EQ(diagnostic->path,
            "/pipeline/" + std::to_string(source_index) + "/outputs/chunks");
  EXPECT_EQ(diagnostic->node_id, "ingress_collision");
  EXPECT_EQ(diagnostic->port, "chunks");
  EXPECT_EQ(diagnostic->related_nodes, std::vector<std::string>({"$ingress"}));
}

TEST_F(ValidatedPipelinePlanTest,
       ResolvesConfiguredPortLifetimeBeforePlanning) {
  const auto boundary = MakeTestBoundary(
      {{"raw_docs", "TextBatch"}, {"raw_queries", "TextBatch"}},
      {{"llm_answers", "TextBatch"},
       {"intent_matches", "RuleMatchBatch"},
       {"doc_chunk_counts", "Int32Batch"}});
  nlohmann::json pipeline_json = {
      {"models",
       nlohmann::json::array({{{"model_id", "embed_model"},
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
             {"inputs", {{"text", "raw_queries"}}},
             {"config",
              {{"bind_model", "embed_model"}, {"lifetime", "session"}}}}})}};

  auto plan = PipelineValidator::ValidateAndPlan(pipeline_json, boundary);
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

TEST_F(ValidatedPipelinePlanTest, NormalizedNodeConfigIsRuntimeSingleSource) {
  nlohmann::json pipeline_json = {
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_0"},
                               {"node_type", PlanTestNode::kNodeType},
                               {"depends_on", nlohmann::json::array()}}})}};

  auto plan =
      PipelineValidator::ValidateAndPlan(pipeline_json, MakeTestBoundary());
  ASSERT_TRUE(plan.report.ok);
  ASSERT_TRUE(plan.node_plans.at("node_0").node.config.empty());
  EXPECT_EQ(plan.node_plans.at("node_0").normalized_config.at("retry_limit"),
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
      {"models", nlohmann::json::array(
                     {{{"model_id", "shared"},
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
       DeterministicLexicalModelPathValidationWithoutDeploymentContext) {
  const auto boundary =
      MakeTestBoundary({{"rerank_queries", "TextBatch"},
                        {"rerank_candidates", "RankedTextBatch", true, "N:1"},
                        {"rerank_pairs", "QueryCandidatesBatch", true, "N:1"}},
                       {{"ranked_results", "RankedTextBatch", true, "N:1"}});
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
      {"models",
       nlohmann::json::array({{{"model_id", "m_rel"},
                               {"model_type", "mock_path_model"},
                               {"backend", "mock_path_backend"},
                               {"model_path", "./models/sub/model.onnx"},
                               {"model_config", nlohmann::json::object()}},
                              {{"model_id", "m_abs"},
                               {"model_type", "mock_path_model"},
                               {"backend", "mock_path_backend"},
                               {"model_path", "/opt/models/fixed.onnx"},
                               {"model_config", nlohmann::json::object()}},
                              {{"model_id", "m_direct"},
                               {"model_type", "mock_path_model"},
                               {"backend", "mock_path_backend"},
                               {"model_path", "model_direct.onnx"},
                               {"model_config", nlohmann::json::object()}}})},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_0_TextRerankNode"},
                               {"node_type", "TextRerankNode"},
                               {"depends_on", nlohmann::json::array()},
                               {"inputs",
                                {{"queries", "rerank_queries"},
                                 {"candidates", "rerank_candidates"}}},
                               {"outputs", {{"ranked", "ranked_results"}}},
                               {"config", {{"bind_model", "m_rel"}}}}})}};

  // 流程编排层只做确定性的词法归一化。部署根目录由接入适配层负责。
  auto plan = PipelineValidator::ValidateAndPlan(pipeline_json, boundary);
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump();
  EXPECT_EQ(plan.models[0].resolved_model_path, "models/sub/model.onnx");
  EXPECT_EQ(plan.models[1].resolved_model_path, "/opt/models/fixed.onnx");
  EXPECT_EQ(plan.models[2].resolved_model_path, "model_direct.onnx");

  auto plan_repeat =
      PipelineValidator::ValidateAndPlan(pipeline_json, boundary);
  ASSERT_TRUE(plan_repeat.report.ok);
  EXPECT_EQ(plan_repeat.models[0].resolved_model_path,
            plan.models[0].resolved_model_path);
  EXPECT_EQ(plan_repeat.models[1].resolved_model_path,
            plan.models[1].resolved_model_path);
  EXPECT_EQ(plan_repeat.models[2].resolved_model_path,
            plan.models[2].resolved_model_path);

  // 即使尚未解析部署，父目录遍历仍然非法。
  nlohmann::json escape_json = pipeline_json;
  escape_json["models"][0]["model_path"] = "../escape.onnx";
  auto plan_escape = PipelineValidator::ValidateAndPlan(escape_json, boundary);
  EXPECT_FALSE(plan_escape.report.ok);
}

TEST_F(ValidatedPipelinePlanTest,
       RejectsIncompatibleEgressPortExecutionContracts) {
  const auto boundary = MakeTestBoundary(
      {}, {{"flow", "TextBatch", true, "1:1", "independent", "session"}});

  nlohmann::json pipeline_json = {
      {"models", nlohmann::json::array()},
      {"pipeline", nlohmann::json::array(
                       {{{"id", "producer"},
                         {"node_type", FlowContractProducerNode::kNodeType},
                         {"depends_on", nlohmann::json::array()}}})}};

  auto plan = PipelineValidator::ValidateAndPlan(pipeline_json, boundary);
  EXPECT_FALSE(plan.report.ok);

  bool found_cardinality = false;
  bool found_provenance = false;
  bool found_lifetime = false;
  for (const auto& diag : plan.report.diagnostics) {
    if (diag.code == DiagnosticCode::kPortCardinalityMismatch) {
      found_cardinality = true;
      EXPECT_EQ(diag.related_nodes, std::vector<std::string>({"$io_output"}));
      EXPECT_EQ(diag.port, "flow");
    } else if (diag.code == DiagnosticCode::kPortProvenanceMismatch) {
      found_provenance = true;
      EXPECT_EQ(diag.related_nodes, std::vector<std::string>({"$io_output"}));
      EXPECT_EQ(diag.port, "flow");
    } else if (diag.code == DiagnosticCode::kPortLifetimeMismatch) {
      found_lifetime = true;
      EXPECT_EQ(diag.related_nodes, std::vector<std::string>({"$io_output"}));
      EXPECT_EQ(diag.port, "flow");
    }
  }
  EXPECT_TRUE(found_cardinality);
  EXPECT_TRUE(found_provenance);
  EXPECT_TRUE(found_lifetime);
}

TEST_F(ValidatedPipelinePlanTest,
       OptionalEgressMayBeAbsentButMustMatchWhenPresent) {
  const auto boundary = MakeTestBoundary(
      {}, {{"optional", "Int32Batch", false, "N:M", "aggregate", "request"}});
  nlohmann::json config = {
      {"pipeline",
       {{{"id", "source"},
         {"node_type", FlowContractProducerNode::kNodeType},
         {"depends_on", nlohmann::json::array()}}}}};
  EXPECT_TRUE(PipelineValidator::ValidateAndPlan(config, boundary).report.ok);
  config["pipeline"][0]["outputs"]["flow"] = "optional";
  auto plan = PipelineValidator::ValidateAndPlan(config, boundary);
  EXPECT_FALSE(plan.report.ok);
  ASSERT_FALSE(plan.report.diagnostics.empty());
  EXPECT_EQ(plan.report.diagnostics.back().code,
            DiagnosticCode::kMissingOutputProducer);
  EXPECT_EQ(plan.report.diagnostics.back().related_nodes,
            std::vector<std::string>{"$io_output"});
}

TEST_F(ValidatedPipelinePlanTest,
       MultiModelBindingsAndConcurrencyDeduplication) {
  nlohmann::json base_pipeline = {
      {"max_parallel_workers", 4},
      {"models", nlohmann::json::array({
                     {{"model_id", "shared_a"},
                      {"model_type", SerializedPlanTestModel::kModelType},
                      {"backend", "test_tensor_backend"},
                      {"model_path", "model_a.bin"},
                      {"model_config", nlohmann::json::object()},
                      {"backend_config", nlohmann::json::object()}},
                     {{"model_id", "independent_b"},
                      {"model_type", SerializedPlanTestModel::kModelType},
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
  auto plan_ok =
      PipelineValidator::ValidateAndPlan(base_pipeline, MakeTestBoundary());
  ASSERT_TRUE(plan_ok.report.ok) << plan_ok.report.ToJson().dump(2);
  EXPECT_EQ(
      plan_ok.report.topological_layers,
      (std::vector<std::vector<std::string>>{{"multi_node", "parallel_node"}}));
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

  // 2. 其他节点共享 serialized 模型时拆到单独的层
  auto conflict_pipeline = base_pipeline;
  conflict_pipeline["pipeline"][1]["config"]["bind_model"] = "shared_a";
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
                       {{"id", "node1"},
                        {"node_type", "IoBoundaryTestNode"},
                        {"depends_on", nlohmann::json::array()},
                        {"inputs", {{"input_data", "text_in"}}},
                        {"outputs", {{"output_data", "text_out"}}}},
                   })}};

  // 1. 合法 IO boundary：覆盖必需 ingress，消费 egress
  PipelineIoBoundary valid_boundary;
  valid_boundary.input_published_ports = {
      IoPortDefinition("text_in", "TextBatch", true)};
  valid_boundary.output_consumed_ports = {
      IoPortDefinition("text_out", "TextBatch", true)};

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
                     return d.code == DiagnosticCode::kMissingInputProducer;
                   });
  ASSERT_NE(diag_in, plan_missing_in.report.diagnostics.end());
  EXPECT_EQ(diag_in->path, "/pipeline/0/inputs/input_data");

  // 3. 缺失输出消费者所需的生产者
  PipelineIoBoundary missing_out_boundary = valid_boundary;
  missing_out_boundary.output_consumed_ports.push_back(
      IoPortDefinition("unproduced_out", "TextBatch", true));
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

  // 4. 输入额外发布与 Pipeline 内部节点输出冲突 (重复写入)
  PipelineIoBoundary conflict_boundary = valid_boundary;
  conflict_boundary.input_published_ports.push_back(
      IoPortDefinition("text_out", "TextBatch", true));
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
  const auto boundary = MakeTestBoundary({{"request", "TextBatch"}});
  nlohmann::json config = {
      {"pipeline",
       {{{"id", "consumer"},
         {"node_type", IoBoundaryTestNode::kNodeType},
         {"inputs", {{"input_data", "intermediate"}}},
         {"outputs", {{"output_data", "result"}}},
         {"depends_on", {"barrier", "producer"}}},
        {{"id", "barrier"},
         {"node_type", PlanTestNode::kNodeType},
         {"depends_on", {"producer"}}},
        {{"id", "producer"},
         {"node_type", IoBoundaryTestNode::kNodeType},
         {"inputs", {{"input_data", "request"}}},
         {"outputs", {{"output_data", "intermediate"}}}}}}};
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
    nlohmann::json first = {{"id", "first"},
                            {"node_type", IoBoundaryTestNode::kNodeType},
                            {"inputs", {{"input_data", "second_out"}}},
                            {"outputs", {{"output_data", "first_out"}}}};
    nlohmann::json second = {{"id", "second"},
                             {"node_type", IoBoundaryTestNode::kNodeType},
                             {"inputs", {{"input_data", "first_out"}}},
                             {"outputs", {{"output_data", "second_out"}}}};
    if (mixed) {
      first["inputs"]["input_data"] = "request";
      first["depends_on"] = {"second"};
    }
    const auto boundary = MakeTestBoundary({{"request", "TextBatch"}});
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
        {{{"id", "self"},
          {"node_type", IoBoundaryTestNode::kNodeType},
          {"inputs", {{"input_data", "shared"}}},
          {"outputs", {{"output_data", "shared"}}}}}}},
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
        {{{"id", "producer"},
          {"node_type", FlowContractProducerNode::kNodeType}},
         {{"id", "consumer"},
          {"node_type", FlowContractConsumerNode::kNodeType},
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
  EXPECT_EQ(diagnostic->path, "/pipeline/1/inputs/flow");
  EXPECT_EQ(diagnostic->node_id, "consumer");
}

TEST_F(ValidatedPipelinePlanTest,
       RejectsAmbiguousProducersWithoutExplicitDependencies) {
  for (bool reverse : {false, true}) {
    nlohmann::json nodes = {{{"id", "consumer"},
                             {"node_type", IoBoundaryTestNode::kNodeType},
                             {"inputs", {{"input_data", "shared"}}},
                             {"outputs", {{"output_data", "result"}}}},
                            {{"id", "first"},
                             {"node_type", FlowContractProducerNode::kNodeType},
                             {"outputs", {{"flow", "shared"}}}},
                            {{"id", "second"},
                             {"node_type", FlowContractProducerNode::kNodeType},
                             {"outputs", {{"flow", "shared"}}}}};
    if (reverse) std::reverse(nodes.begin(), nodes.end());
    const auto plan = PipelineValidator::ValidateAndPlan({{"pipeline", nodes}},
                                                         MakeTestBoundary());
    EXPECT_FALSE(plan.report.ok);
    EXPECT_TRUE(std::any_of(
        plan.report.diagnostics.begin(), plan.report.diagnostics.end(),
        [](const auto& d) {
          return d.code == DiagnosticCode::kDuplicatePortProducer;
        }))
        << plan.report.ToJson().dump(2);
  }
}

TEST_F(ValidatedPipelinePlanTest,
       ModelBindingIsRequiredEvenForOneMatchingModel) {
  const auto plan = PipelineValidator::ValidateAndPlan(
      {{"models",
        {{{"model_id", "shared"},
          {"model_type", SerializedPlanTestModel::kModelType},
          {"backend", "test_tensor_backend"},
          {"model_path", "fixture.bin"}}}},
       {"pipeline",
        {{{"id", "consumer"},
          {"node_type", ModelBoundPlanTestNode::kNodeType}}}}},
      MakeTestBoundary());
  EXPECT_FALSE(plan.report.ok);
  EXPECT_TRUE(std::any_of(
      plan.report.diagnostics.begin(), plan.report.diagnostics.end(),
      [](const auto& d) {
        return d.code == DiagnosticCode::kMissingConfigField &&
               d.path == "/pipeline/0/config/bind_model";
      }))
      << plan.report.ToJson().dump(2);
}

TEST_F(ValidatedPipelinePlanTest,
       WorkerBudgetSchedulesUnsafeNodesSequentially) {
  nlohmann::json config = {
      {"pipeline",
       {{{"id", "source"}, {"node_type", FlowContractProducerNode::kNodeType}},
        {{"id", "independent"}, {"node_type", PlanTestNode::kNodeType}}}}};
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

TEST_F(ValidatedPipelinePlanTest,
       SerializationPreservesOriginalLayerWriteChecks) {
  const nlohmann::json config = {
      {"max_parallel_workers", 2},
      {"pipeline",
       {{{"id", "left"}, {"node_type", FlowContractProducerNode::kNodeType}},
        {{"id", "right"},
         {"node_type", FlowContractProducerNode::kNodeType}}}}};
  const auto report = PipelineValidator::Validate(config, MakeTestBoundary());
  EXPECT_FALSE(report.ok);
  EXPECT_TRUE(std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
                          [](const auto& d) {
                            return d.code ==
                                   DiagnosticCode::kParallelWriteConflict;
                          }))
      << report.ToJson().dump(2);
}

TEST_F(ValidatedPipelinePlanTest, PipelineBuildFromPlanLifecycle) {
  nlohmann::json valid_pipeline = {
      {"pipeline", nlohmann::json::array({
                       {{"id", "node1"},
                        {"node_type", "IoBoundaryTestNode"},
                        {"depends_on", nlohmann::json::array()},
                        {"inputs", {{"input_data", "text_in"}}},
                        {"outputs", {{"output_data", "text_out"}}}},
                   })}};

  PipelineIoBoundary boundary;
  boundary.input_published_ports = {
      IoPortDefinition("text_in", "TextBatch", true)};
  boundary.output_consumed_ports = {
      IoPortDefinition("text_out", "TextBatch", true)};

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

}  // namespace llm_edgeflow

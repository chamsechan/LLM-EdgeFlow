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
#include "engine/engine_interface.h"
#include "engine/engine_registry.h"

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
      {DiagnosticCode::kInternalException, "INTERNAL_EXCEPTION"},
  };

  std::unordered_set<std::string> names;
  for (const auto& item : cases) {
    std::string name = DiagnosticCodeName(item.code);
    EXPECT_STREQ(name.c_str(), item.expected_name);
    EXPECT_TRUE(names.insert(name).second) << "Duplicate name: " << name;
  }
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

TEST(ValidatedPipelinePlanTest, RejectsNodeFromDifferentBusiness) {
  nlohmann::json pipeline_json = {
      {"business_name", "smart_doc_qa_v1"},
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array({{{"id", "wrong_business_node"},
                               {"node_type", "KeywordMatcherNode"},
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

}  // namespace alg_framework

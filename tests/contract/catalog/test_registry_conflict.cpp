#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

#include "core/node_interface.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/pipeline_diagnostic.h"
#include "core/pipeline_validator.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"
#include "nodes/parameter_binding.h"

namespace llm_edgeflow {

inline NodeDefinition MakeTestNodeDef(const std::string& type) {
  NodeDefinition def;
  def.node_type = type;
  def.category = "test";
  def.description = "test node " + type;
  def.parallel_safe = true;
  return def;
}

inline ModelDefinition MakeTestModelDef(const std::string& type) {
  ModelDefinition def;
  def.model_type = type;
  def.capability = "embedding";
  def.description = "test model " + type;
  def.required_protocol = ExecutionProtocol::kTensorGraph;
  def.concurrency = InferenceConcurrency::kConcurrent;
  return def;
}

class DummyNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "DummyNode";
  bool Init(const NodeInitContext&) override { return true; }
  int Process(AlgContext*) override { return 0; }
  NodeControlResult Control(int, const std::string&) override {
    return NodeControlResult::Handled(0);
  }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};
REGISTER_NODE_WITH_DEFINITION(DummyNode, MakeTestNodeDef(DummyNode::kNodeType));

namespace {
struct BadAuthoringParameters {
  int count = 0;
};

// This declaration executes before main. Each CTest selects a separate process
// so an authoring failure cannot contaminate the existing registry scenarios.
const bool kAuthoringStartupAttempted = [] {
  const char* selected = std::getenv("EDGEFLOW_BAD_AUTHORING_CASE");
  if (!selected) return false;
  NodeRegistry::Instance().RegisterWithDefinitionFactory(
      "BadAuthoringNode", [] { return std::make_unique<DummyNode>(); },
      [selected] {
        const std::string scenario(selected);
        if (scenario == "invalid_default") {
          const Parameters<BadAuthoringParameters> parameters({
              Field("count", &BadAuthoringParameters::count)
                  .Default(-1)
                  .Minimum(0),
          });
          (void)parameters;
        } else if (scenario == "duplicate_member") {
          const Parameters<BadAuthoringParameters> parameters({
              Field("count", &BadAuthoringParameters::count).Required(),
              Field("other", &BadAuthoringParameters::count).Required(),
          });
          (void)parameters;
        } else if (scenario == "factory_exception") {
          throw std::runtime_error("authoring factory deliberately failed");
        }
        return MakeTestNodeDef("BadAuthoringNode");
      });
  return true;
}();
}  // namespace

TEST(RegistryAuthoringStartupTest,
     DeclarationFailureReachesMainAndFailsClosed) {
  const char* selected = std::getenv("EDGEFLOW_BAD_AUTHORING_CASE");
  if (!selected) GTEST_SKIP() << "Requires a process-isolated authoring case";
  ASSERT_TRUE(kAuthoringStartupAttempted);
  const std::string scenario(selected);
  std::string reason;
  if (scenario == "invalid_default") {
    reason = "Default value for field 'count' is below minimum";
  } else if (scenario == "duplicate_member") {
    reason =
        "Same struct member bound to multiple config fields (count, other)";
  } else {
    ASSERT_EQ(scenario, "factory_exception");
    reason = "authoring factory deliberately failed";
  }
  EXPECT_FALSE(NodeRegistry::Instance().Has("BadAuthoringNode"));
  EXPECT_EQ(NodeRegistry::Instance().Create("BadAuthoringNode"), nullptr);
  ASSERT_TRUE(NodeRegistry::Instance().HasConflict());

  const nlohmann::json config = {
      {"biz_name", "authoring_startup_test"},
      {"pipeline",
       nlohmann::json::array({{{"id", "dummy"},
                               {"node_type", DummyNode::kNodeType},
                               {"depends_on", nlohmann::json::array()}}})}};
  const auto validation = PipelineValidator::ValidateAndPlan(
      config, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_FALSE(validation.report.ok);
  ASSERT_FALSE(validation.report.diagnostics.empty());
  const auto& diagnostic = validation.report.diagnostics.front();
  EXPECT_EQ(diagnostic.code, DiagnosticCode::kRegistryConflict);
  EXPECT_NE(diagnostic.message.find("BadAuthoringNode"), std::string::npos);
  EXPECT_NE(diagnostic.message.find(reason), std::string::npos);

  Pipeline pipeline;
  PipelineDiagnostic build_diagnostic;
  EXPECT_FALSE(
      pipeline.BuildFromJson(config, &build_diagnostic,
                             ValidationPolicy::kPrivateExtensionCompatible));
  EXPECT_EQ(build_diagnostic.code, DiagnosticCode::kRegistryConflict);
  EXPECT_NE(build_diagnostic.message.find("BadAuthoringNode"),
            std::string::npos);
  EXPECT_NE(build_diagnostic.message.find(reason), std::string::npos);
}

class DummyModel : public IEmbeddingModel {
 public:
  inline static constexpr char kModelType[] = "dummy_model";
  static std::shared_ptr<IModel> Create(const ModelCreateContext&,
                                        std::string*) {
    return std::make_shared<DummyModel>();
  }
  size_t GetMaxBatchSize() const noexcept override { return 1; }
  const std::string& ModelType() const noexcept override {
    static const std::string type = kModelType;
    return type;
  }
  const std::string& Capability() const noexcept override {
    static const std::string capability = "embedding";
    return capability;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  int Embed(const TextBatch&, const EmbeddingOptions&,
            EmbeddingBatch*) noexcept override {
    return 0;
  }
};
REGISTER_MODEL_WITH_DEFINITION(DummyModel,
                               MakeTestModelDef(DummyModel::kModelType));

TEST(RegistryConflictNodeTest, DuplicateNodeFailClosed) {
  ASSERT_FALSE(NodeRegistry::Instance().HasConflict());
  ASSERT_FALSE(ModelRegistry::Instance().HasConflict());
  EXPECT_FALSE(NodeRegistry::Instance().Register(
      DummyNode::kNodeType, []() { return std::make_unique<DummyNode>(); },
      MakeTestNodeDef(DummyNode::kNodeType)));
  EXPECT_TRUE(NodeRegistry::Instance().HasConflict());
  EXPECT_NE(NodeRegistry::Instance().Create(DummyNode::kNodeType), nullptr);

  Pipeline pipe;
  PipelineDiagnostic diag;
  nlohmann::json cfg = {
      {"biz_name", "conflict_node_test"},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_0_DummyNode"},
                               {"node_type", DummyNode::kNodeType},
                               {"depends_on", nlohmann::json::array()}}})}};
  EXPECT_FALSE(pipe.BuildFromJson(
      cfg, &diag, ValidationPolicy::kPrivateExtensionCompatible));
  EXPECT_EQ(diag.code, DiagnosticCode::kRegistryConflict);
  EXPECT_EQ(diag.path, "/pipeline");
}

TEST(RegistryConflictModelTest, DuplicateModelFailClosed) {
  ASSERT_FALSE(NodeRegistry::Instance().HasConflict());
  ASSERT_FALSE(ModelRegistry::Instance().HasConflict());
  EXPECT_FALSE(ModelRegistry::Instance().Register(
      MakeTestModelDef(DummyModel::kModelType), DummyModel::Create));
  EXPECT_TRUE(ModelRegistry::Instance().HasConflict());

  Pipeline pipe;
  PipelineDiagnostic diag;
  nlohmann::json cfg = {
      {"biz_name", "conflict_model_test"},
      {"models",
       nlohmann::json::array({{{"model_id", "m1"},
                               {"capability", "embedding"},
                               {"model_type", DummyModel::kModelType},
                               {"backend", "unused_backend"},
                               {"model_path", "unused.bin"},
                               {"model_config", nlohmann::json::object()},
                               {"backend_config", nlohmann::json::object()}}})},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_0_DummyNode"},
                               {"node_type", DummyNode::kNodeType},
                               {"depends_on", nlohmann::json::array()}}})}};
  EXPECT_FALSE(pipe.BuildFromJson(
      cfg, &diag, ValidationPolicy::kPrivateExtensionCompatible));
  EXPECT_EQ(diag.code, DiagnosticCode::kRegistryConflict);
  EXPECT_EQ(diag.path, "/models");
}

}  // namespace llm_edgeflow

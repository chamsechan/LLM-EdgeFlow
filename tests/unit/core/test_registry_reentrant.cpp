#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "core/node_interface.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/pipeline_diagnostic.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"
#include "tests/support/registry_test_access.h"
#include "tests/support/scoped_allocation_failure.h"

namespace llm_edgeflow {

static NodeDefinition MakeTestNodeDef(const std::string& type) {
  NodeDefinition def;
  def.node_type = type;
  def.category = "test";
  def.description = "test node " + type;
  def.parallel_safe = true;
  return def;
}

static ModelDefinition MakeTestModelDef(const std::string& type) {
  ModelDefinition def;
  def.model_type = type;
  def.capability = "embedding";
  def.description = "test model " + type;
  def.required_protocol = ExecutionProtocol::kTensorGraph;
  def.concurrency = InferenceConcurrency::kConcurrent;
  return def;
}

// RECHECK-R1-003: 构造期重入自身 Registry 查询，测试锁粒度是否正确释放
class ReentrantNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "ReentrantNode";
  ReentrantNode() {
    // 构造期间同步调用 NodeRegistry 查询
    volatile bool has = NodeRegistry::Instance().Has("ReentrantNode");
    (void)has;
  }
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
REGISTER_NODE_WITH_DEFINITION(ReentrantNode,
                              MakeTestNodeDef(ReentrantNode::kNodeType));

class ReentrantModel : public IEmbeddingModel {
 public:
  inline static constexpr char kModelType[] = "reentrant_model";

  ReentrantModel() {
    // 构造期间同步调用 ModelRegistry 查询
    volatile bool has = ModelRegistry::Instance().Has(kModelType);
    (void)has;
  }
  static std::shared_ptr<IModel> Create(const ModelCreateContext&,
                                        std::string*) {
    return std::make_shared<ReentrantModel>();
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
REGISTER_MODEL_WITH_DEFINITION(ReentrantModel,
                               MakeTestModelDef(ReentrantModel::kModelType));

TEST(RegistryReentrantTest, ReentrantCreationZeroDeadlock) {
  // 1. 同步测试 Node 构造期重入 NodeRegistry
  {
    Pipeline p;
    PipelineDiagnostic diag;
    nlohmann::json cfg = {
        {"biz_name", "reentrant_node_test"},
        {"pipeline",
         nlohmann::json::array({{{"id", "node_0_ReentrantNode"},
                                 {"node_type", "ReentrantNode"},
                                 {"depends_on", nlohmann::json::array()}}})}};
    EXPECT_TRUE(p.BuildFromJson(cfg, &diag,
                                ValidationPolicy::kPrivateExtensionCompatible));
    EXPECT_TRUE(p.IsReady());
  }

  // 2. 同步测试 Model 构造期重入 ModelRegistry
  {
    Pipeline p;
    PipelineDiagnostic diag;
    nlohmann::json cfg = {
        {"biz_name", "reentrant_model_test"},
        {"models", nlohmann::json::array(
                       {{{"model_id", "m1"},
                         {"capability", "embedding"},
                         {"model_type", ReentrantModel::kModelType},
                         {"backend", "test_tensor_backend"},
                         {"model_path", "reentrant.bin"},
                         {"model_config", nlohmann::json::object()},
                         {"backend_config", nlohmann::json::object()}}})},
        {"pipeline",
         nlohmann::json::array({{{"id", "node_0_ReentrantNode"},
                                 {"node_type", "ReentrantNode"},
                                 {"depends_on", nlohmann::json::array()}}})}};
    EXPECT_TRUE(p.BuildFromJson(cfg, &diag,
                                ValidationPolicy::kPrivateExtensionCompatible));
    EXPECT_TRUE(p.IsReady());
  }
}

class SimpleTestNode : public INode {
 public:
  bool Init(const NodeInitContext&) override { return true; }
  int Process(AlgContext*) override { return 0; }
  NodeControlResult Control(int, const std::string&) override {
    return NodeControlResult::Handled(0);
  }
  const std::string& Name() const override {
    static const std::string name = "SimpleTestNode";
    return name;
  }
};

// R5: creator execution, creator copy, Definition callback copy, reentrant
// queries
TEST(RegistryReentrantTest, ReentrantCreatorAndFactoryZeroDeadlock) {
  test_support::RegistryTestAccess::ScopedNodeState scoped;
  bool factory_invoked = false;
  bool registered = NodeRegistry::Instance().RegisterWithDefinitionFactory(
      "ReentrantFactoryNode",
      []() -> std::unique_ptr<INode> {
        EXPECT_TRUE(NodeRegistry::Instance().Has("ReentrantFactoryNode"));
        auto found = NodeRegistry::Instance().Find("ReentrantFactoryNode");
        EXPECT_TRUE(found.has_value());
        auto list = NodeRegistry::Instance().ListDefinitions();
        EXPECT_FALSE(list.empty());
        auto snap = NodeRegistry::Instance().Snapshot();
        EXPECT_FALSE(snap.definitions.empty());
        return std::make_unique<SimpleTestNode>();
      },
      [&]() -> NodeDefinition {
        factory_invoked = true;
        (void)NodeRegistry::Instance().Has("ReentrantNode");
        (void)NodeRegistry::Instance().ListDefinitions();
        (void)NodeRegistry::Instance().Snapshot();
        return MakeTestNodeDef("ReentrantFactoryNode");
      });
  EXPECT_TRUE(registered);
  EXPECT_TRUE(factory_invoked);

  auto instance = NodeRegistry::Instance().Create("ReentrantFactoryNode");
  EXPECT_NE(instance, nullptr);
}

// R4: Register fail-after-N loop with ScopedAllocationFailure
TEST(RegistryReentrantTest, RegisterFailAfterNIntegrity) {
  test_support::RegistryTestAccess::ScopedNodeState scoped;
  const std::string sentinel = "AllocFailSentinelNode";
  ASSERT_TRUE(NodeRegistry::Instance().Register(
      sentinel, []() { return std::make_unique<SimpleTestNode>(); },
      MakeTestNodeDef(sentinel)));
  ASSERT_FALSE(NodeRegistry::Instance().HasConflict());

  bool completed = false;
  for (int step = 0; step < 20; ++step) {
    const std::string candidate = "AllocFailProbe_" + std::to_string(step);
    NodeRegistry::CreatorFunc creator = []() {
      return std::make_unique<SimpleTestNode>();
    };
    NodeDefinition def = MakeTestNodeDef(candidate);
    bool registered = false;
    bool injected = false;
    size_t outstanding = 0;
    bool overflowed = false;
    {
      test_support::ScopedAllocationFailure failure(step);
      registered = NodeRegistry::Instance().Register(candidate,
                                                     std::move(creator), &def);
      injected = failure.Triggered();
      outstanding = failure.Outstanding();
      overflowed = failure.Overflowed();
    }
    ASSERT_FALSE(overflowed);
    if (!injected) {
      EXPECT_TRUE(registered);
      EXPECT_TRUE(NodeRegistry::Instance().Has(candidate));
      EXPECT_NE(NodeRegistry::Instance().Create(candidate), nullptr);
      completed = true;
      break;
    }
    EXPECT_FALSE(registered);
    EXPECT_FALSE(NodeRegistry::Instance().Has(candidate));
    EXPECT_EQ(NodeRegistry::Instance().Create(candidate), nullptr);
    EXPECT_TRUE(NodeRegistry::Instance().HasConflict());
    EXPECT_TRUE(NodeRegistry::Instance().Has(sentinel));
    EXPECT_NE(NodeRegistry::Instance().Create(sentinel), nullptr);
    EXPECT_LE(outstanding, 2u);

    test_support::RegistryTestAccess::ClearNodeFailures();
  }
  EXPECT_TRUE(completed);
}

// R4: RegisterWithDefinitionFactory fail-after-N loop with
// ScopedAllocationFailure
TEST(RegistryReentrantTest, RegisterWithDefinitionFactoryFailAfterNIntegrity) {
  test_support::RegistryTestAccess::ScopedNodeState scoped;
  const std::string sentinel = "FactoryAllocFailSentinelNode";
  ASSERT_TRUE(NodeRegistry::Instance().Register(
      sentinel, []() { return std::make_unique<SimpleTestNode>(); },
      MakeTestNodeDef(sentinel)));
  ASSERT_FALSE(NodeRegistry::Instance().HasConflict());

  bool completed = false;
  for (int step = 0; step < 20; ++step) {
    const std::string candidate =
        "FactoryAllocFailProbe_" + std::to_string(step);
    bool registered = false;
    bool injected = false;
    size_t outstanding = 0;
    bool overflowed = false;
    {
      test_support::ScopedAllocationFailure failure(step);
      registered = NodeRegistry::Instance().RegisterWithDefinitionFactory(
          candidate, []() { return std::make_unique<SimpleTestNode>(); },
          [&]() { return MakeTestNodeDef(candidate); });
      injected = failure.Triggered();
      outstanding = failure.Outstanding();
      overflowed = failure.Overflowed();
    }
    ASSERT_FALSE(overflowed);
    if (!injected) {
      EXPECT_TRUE(registered);
      EXPECT_TRUE(NodeRegistry::Instance().Has(candidate));
      EXPECT_NE(NodeRegistry::Instance().Create(candidate), nullptr);
      completed = true;
      break;
    }
    EXPECT_FALSE(registered);
    EXPECT_FALSE(NodeRegistry::Instance().Has(candidate));
    EXPECT_EQ(NodeRegistry::Instance().Create(candidate), nullptr);
    EXPECT_TRUE(NodeRegistry::Instance().HasConflict());
    EXPECT_TRUE(NodeRegistry::Instance().Has(sentinel));
    EXPECT_NE(NodeRegistry::Instance().Create(sentinel), nullptr);
    EXPECT_LE(outstanding, 2u);

    test_support::RegistryTestAccess::ClearNodeFailures();
  }
  EXPECT_TRUE(completed);
}

}  // namespace llm_edgeflow

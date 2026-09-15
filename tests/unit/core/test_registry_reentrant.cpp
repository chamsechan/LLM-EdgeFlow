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

// R5: creator copy and Definition callback copy reentrancy without deadlock
struct ReentrantCopyCreator {
  static inline std::atomic<int> copy_count{0};

  ReentrantCopyCreator() = default;
  ReentrantCopyCreator(const ReentrantCopyCreator& /*other*/) {
    copy_count.fetch_add(1, std::memory_order_relaxed);
    (void)NodeRegistry::Instance().Has("ReentrantCallableCopyNode");
    (void)NodeRegistry::Instance().ListTypes();
    (void)NodeRegistry::Instance().Snapshot();
  }
  ReentrantCopyCreator(ReentrantCopyCreator&& other)
      : ReentrantCopyCreator(other) {}
  ReentrantCopyCreator& operator=(const ReentrantCopyCreator&) = default;
  ReentrantCopyCreator& operator=(ReentrantCopyCreator&&) = default;

  std::unique_ptr<INode> operator()() const {
    return std::make_unique<SimpleTestNode>();
  }
};

struct ReentrantConfigValidator {
  static inline std::atomic<int> copy_count{0};

  ReentrantConfigValidator() = default;
  ReentrantConfigValidator(const ReentrantConfigValidator& /*other*/) {
    copy_count.fetch_add(1, std::memory_order_relaxed);
    (void)NodeRegistry::Instance().Has("ReentrantCallableCopyNode");
    (void)NodeRegistry::Instance().ListTypes();
  }
  ReentrantConfigValidator(ReentrantConfigValidator&& other)
      : ReentrantConfigValidator(other) {}
  ReentrantConfigValidator& operator=(const ReentrantConfigValidator&) =
      default;
  ReentrantConfigValidator& operator=(ReentrantConfigValidator&&) = default;

  bool operator()(const nlohmann::json&, const std::unordered_set<std::string>&,
                  std::string*) const {
    return true;
  }
};

struct ReentrantCopyFactory {
  static inline std::atomic<int> copy_count{0};

  ReentrantCopyFactory() = default;
  ReentrantCopyFactory(const ReentrantCopyFactory& /*other*/) {
    copy_count.fetch_add(1, std::memory_order_relaxed);
    (void)NodeRegistry::Instance().Has("ReentrantCallableCopyNode");
    (void)NodeRegistry::Instance().ListTypes();
    (void)NodeRegistry::Instance().Snapshot();
  }
  ReentrantCopyFactory(ReentrantCopyFactory&& other)
      : ReentrantCopyFactory(other) {}
  ReentrantCopyFactory& operator=(const ReentrantCopyFactory&) = default;
  ReentrantCopyFactory& operator=(ReentrantCopyFactory&&) = default;

  NodeDefinition operator()() const {
    NodeDefinition def = MakeTestNodeDef("ReentrantCallableCopyNode");
    def.validate_config = ReentrantConfigValidator{};
    return def;
  }
};

TEST(RegistryReentrantTest, ReentrantCallableCopyZeroDeadlock) {
  test_support::RegistryTestAccess::ScopedNodeState scoped;
  ReentrantCopyCreator::copy_count.store(0);
  ReentrantConfigValidator::copy_count.store(0);
  ReentrantCopyFactory::copy_count.store(0);

  ReentrantCopyCreator creator;
  ReentrantCopyFactory factory;

  // Pass factory wrapped in std::function to explicitly exercise factory
  // callable copy reentrancy
  std::function<NodeDefinition()> factory_wrapper = factory;

  bool registered = NodeRegistry::Instance().RegisterWithDefinitionFactory(
      "ReentrantCallableCopyNode", creator, factory_wrapper);
  EXPECT_TRUE(registered);
  EXPECT_TRUE(NodeRegistry::Instance().Has("ReentrantCallableCopyNode"));

  // Create node: copies handle->creator outside registry lock, triggers
  // ReentrantCopyCreator copy
  auto instance = NodeRegistry::Instance().Create("ReentrantCallableCopyNode");
  EXPECT_NE(instance, nullptr);
  EXPECT_GT(ReentrantCopyCreator::copy_count.load(), 0);

  // Snapshot: copies handle->definition outside registry lock, triggers
  // ReentrantConfigValidator copy
  auto snap = NodeRegistry::Instance().Snapshot();
  EXPECT_FALSE(snap.definitions.empty());
  EXPECT_GT(ReentrantConfigValidator::copy_count.load(), 0);

  // ListDefinitions: also copies definitions outside lock
  auto list = NodeRegistry::Instance().ListDefinitions();
  EXPECT_FALSE(list.empty());

  EXPECT_GT(ReentrantCopyFactory::copy_count.load(), 0);
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

// R4/R5: Insertion allocation failure with user resource destructor calling
// NodeRegistry::Has() must not deadlock on mutex_.
struct ReentrantDestructorResource {
  static inline std::atomic<int> destruct_count{0};
  static inline std::atomic<int> reentrant_has_count{0};

  ~ReentrantDestructorResource() {
    destruct_count.fetch_add(1, std::memory_order_relaxed);
    static const std::string sentinel = "AllocFailSentinelNode";
    if (NodeRegistry::Instance().Has(sentinel)) {
      auto created = NodeRegistry::Instance().Create(sentinel);
      auto types = NodeRegistry::Instance().ListTypes();
      auto found = NodeRegistry::Instance().Find(sentinel);
      if (created != nullptr && !types.empty() && found.has_value()) {
        reentrant_has_count.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
};

TEST(RegistryReentrantTest,
     RegisterAllocationFailureReentrantDestructorZeroDeadlock) {
  test_support::RegistryTestAccess::ScopedNodeState scoped;
  ReentrantDestructorResource::destruct_count.store(0);
  ReentrantDestructorResource::reentrant_has_count.store(0);

  const std::string sentinel = "AllocFailSentinelNode";
  ASSERT_TRUE(NodeRegistry::Instance().Register(
      sentinel, []() { return std::make_unique<SimpleTestNode>(); },
      MakeTestNodeDef(sentinel)));
  ASSERT_FALSE(NodeRegistry::Instance().HasConflict());

  bool completed = false;
  int failure_steps_tested = 0;
  for (int step = 0; step < 20; ++step) {
    const std::string candidate =
        "AllocFailReentrantProbe_" + std::to_string(step);
    auto resource = std::make_shared<ReentrantDestructorResource>();
    NodeRegistry::CreatorFunc creator =
        [res = resource]() -> std::unique_ptr<INode> {
      (void)res;
      return std::make_unique<SimpleTestNode>();
    };
    resource.reset();

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
    failure_steps_tested++;
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
  EXPECT_GE(failure_steps_tested, 5);
  EXPECT_GE(ReentrantDestructorResource::destruct_count.load(), 5);
  EXPECT_EQ(ReentrantDestructorResource::destruct_count.load(),
            ReentrantDestructorResource::reentrant_has_count.load());
}

// R4: Register duplicate fail-after-N loop with ScopedAllocationFailure
TEST(RegistryReentrantTest, RegisterDuplicateFailAfterNIntegrity) {
  test_support::RegistryTestAccess::ScopedNodeState scoped;
  const std::string existing = "AllocFailDuplicateNode";
  ASSERT_TRUE(NodeRegistry::Instance().Register(
      existing, []() { return std::make_unique<SimpleTestNode>(); },
      MakeTestNodeDef(existing)));
  ASSERT_FALSE(NodeRegistry::Instance().HasConflict());

  bool completed = false;
  for (int step = 0; step < 20; ++step) {
    NodeRegistry::CreatorFunc creator = []() {
      return std::make_unique<SimpleTestNode>();
    };
    NodeDefinition def = MakeTestNodeDef(existing);
    bool registered = false;
    bool injected = false;
    size_t outstanding = 0;
    bool overflowed = false;
    {
      test_support::ScopedAllocationFailure failure(step);
      registered =
          NodeRegistry::Instance().Register(existing, std::move(creator), &def);
      injected = failure.Triggered();
      outstanding = failure.Outstanding();
      overflowed = failure.Overflowed();
    }
    ASSERT_FALSE(overflowed);
    EXPECT_FALSE(registered);
    EXPECT_TRUE(NodeRegistry::Instance().HasConflict());
    EXPECT_TRUE(NodeRegistry::Instance().Has(existing));
    EXPECT_NE(NodeRegistry::Instance().Create(existing), nullptr);
    EXPECT_LE(outstanding, 2u);

    if (!injected) {
      completed = true;
      break;
    }
    test_support::RegistryTestAccess::ClearNodeFailures();
  }
  EXPECT_TRUE(completed);
}

// R4: Register cross-node control conflict fail-after-N loop with
// ScopedAllocationFailure
TEST(RegistryReentrantTest, RegisterControlConflictFailAfterNIntegrity) {
  test_support::RegistryTestAccess::ScopedNodeState scoped;
  const std::string node_a = "AllocFailControlNodeA";
  NodeDefinition def_a = MakeTestNodeDef(node_a);
  ControlCommandDefinition cmd_a;
  cmd_a.cmd_id = 999;
  cmd_a.name = "conflict_cmd";
  cmd_a.shared_id = false;
  cmd_a.payload_schema = nlohmann::json::object();
  cmd_a.supports_hot_swap = false;
  def_a.control_commands = {cmd_a};

  ASSERT_TRUE(NodeRegistry::Instance().Register(
      node_a, []() { return std::make_unique<SimpleTestNode>(); }, def_a));
  ASSERT_FALSE(NodeRegistry::Instance().HasConflict());

  const std::string node_b = "AllocFailControlNodeB";
  NodeDefinition def_b = MakeTestNodeDef(node_b);
  ControlCommandDefinition cmd_b;
  cmd_b.cmd_id = 999;  // Conflict with node_a command
  cmd_b.name = "different_name";
  cmd_b.shared_id = false;
  cmd_b.payload_schema = nlohmann::json::object();
  cmd_b.supports_hot_swap = false;
  def_b.control_commands = {cmd_b};

  bool completed = false;
  for (int step = 0; step < 20; ++step) {
    NodeRegistry::CreatorFunc creator = []() {
      return std::make_unique<SimpleTestNode>();
    };
    NodeDefinition def = def_b;
    bool registered = false;
    bool injected = false;
    size_t outstanding = 0;
    bool overflowed = false;
    {
      test_support::ScopedAllocationFailure failure(step);
      registered =
          NodeRegistry::Instance().Register(node_b, std::move(creator), &def);
      injected = failure.Triggered();
      outstanding = failure.Outstanding();
      overflowed = failure.Overflowed();
    }
    ASSERT_FALSE(overflowed);
    EXPECT_FALSE(registered);
    EXPECT_TRUE(NodeRegistry::Instance().HasConflict());
    EXPECT_FALSE(NodeRegistry::Instance().Has(node_b));
    EXPECT_EQ(NodeRegistry::Instance().Create(node_b), nullptr);
    EXPECT_TRUE(NodeRegistry::Instance().Has(node_a));
    EXPECT_NE(NodeRegistry::Instance().Create(node_a), nullptr);
    EXPECT_LE(outstanding, 2u);

    if (!injected) {
      completed = true;
      break;
    }
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

// R8: ScopedNodeState restoration under allocation failure is non-allocating
// and robust
TEST(RegistryReentrantTest, ScopedNodeStateRestorationUnderAllocationFailure) {
  const std::string sentinel = "RestorationSentinelNode";
  {
    test_support::RegistryTestAccess::ScopedNodeState outer;
    ASSERT_TRUE(NodeRegistry::Instance().Register(
        sentinel, []() { return std::make_unique<SimpleTestNode>(); },
        MakeTestNodeDef(sentinel)));
    ASSERT_FALSE(NodeRegistry::Instance().HasConflict());

    {
      std::optional<test_support::ScopedAllocationFailure> failure;
      {
        test_support::RegistryTestAccess::ScopedNodeState inner;
        const std::string temp_node = "TemporaryNodeToRestore";
        ASSERT_TRUE(NodeRegistry::Instance().Register(
            temp_node, []() { return std::make_unique<SimpleTestNode>(); },
            MakeTestNodeDef(temp_node)));
        EXPECT_TRUE(NodeRegistry::Instance().Has(temp_node));

        // Trigger conflict so has_conflict_ is true
        EXPECT_FALSE(NodeRegistry::Instance().Register(
            temp_node, []() { return std::make_unique<SimpleTestNode>(); },
            MakeTestNodeDef(temp_node)));
        EXPECT_TRUE(NodeRegistry::Instance().HasConflict());

        // Arm allocation failure so that ~ScopedNodeState runs with failure(0)
        failure.emplace(0);
      }
      // inner is destructed while failure is active, then failure is destructed
      ASSERT_TRUE(failure.has_value());
      EXPECT_FALSE(failure->Triggered());
    }

    // After inner scope destroyed under allocation failure, registry must be
    // cleanly restored
    EXPECT_FALSE(NodeRegistry::Instance().HasConflict());
    EXPECT_TRUE(NodeRegistry::Instance().Has(sentinel));
    EXPECT_NE(NodeRegistry::Instance().Create(sentinel), nullptr);
    EXPECT_FALSE(NodeRegistry::Instance().Has("TemporaryNodeToRestore"));
    EXPECT_EQ(NodeRegistry::Instance().Create("TemporaryNodeToRestore"),
              nullptr);
  }
}

}  // namespace llm_edgeflow

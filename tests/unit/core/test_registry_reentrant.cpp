#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/node_registry.h"
#include "tests/support/registry_test_access.h"

namespace llm_edgeflow {
namespace {

class RegistryProbeNode final : public INode {
 public:
  bool Init(const NodeInitContext&) override { return true; }
  int Process(AlgContext*) override { return 0; }
  const std::string& Name() const override {
    static const std::string name = "registry_probe";
    return name;
  }
};

NodeDefinition Definition(std::string type, int control = 0) {
  NodeDefinition definition;
  definition.node_type = std::move(type);
  definition.description = "literal description";
  definition.inputs = {NodePortDefinition{"text", "TextBatch"}};
  definition.outputs = {NodePortDefinition{"answer", "TextBatch"}};
  if (control) definition.control_commands.emplace_back(control, "update");
  return definition;
}

auto Creator() {
  return [] { return std::make_unique<RegistryProbeNode>(); };
}

class NodeRegistryTest : public ::testing::Test {
 protected:
  void SetUp() override { test_support::RegistryTestAccess::ResetNodes(); }
  test_support::RegistryTestAccess::ScopedNodeState saved_;
};

TEST_F(NodeRegistryTest,
       RegistrationAndReadSnapshotsAreIndependentSortedValues) {
  auto& registry = NodeRegistry::Instance();
  auto original = Definition("zeta");
  ASSERT_TRUE(registry.Register("zeta", Creator(), original));
  ASSERT_TRUE(registry.Register("alpha", Creator(), Definition("alpha")));
  original.description = "mutated original";
  auto snapshot = registry.Snapshot();
  ASSERT_EQ(snapshot.definitions.size(), 2U);
  EXPECT_EQ(snapshot.definitions[0].node_type, "alpha");
  EXPECT_EQ(snapshot.definitions[1].node_type, "zeta");
  EXPECT_FALSE(snapshot.has_conflict);
  EXPECT_TRUE(snapshot.conflict_errors.empty());
  auto found = registry.Find("zeta");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->description, "literal description");
  found->description = "mutated Find result";
  snapshot.definitions[1].description = "mutated Snapshot result";
  EXPECT_EQ(registry.Find("zeta")->description, "literal description");
  const auto definitions = registry.ListDefinitions();
  ASSERT_EQ(definitions.size(), 2U);
  EXPECT_EQ(definitions[0].node_type, "alpha");
  EXPECT_EQ(definitions[1].node_type, "zeta");
  EXPECT_EQ(registry.Create("unknown"), nullptr);
  EXPECT_NE(registry.Create("alpha"), nullptr);
}

TEST_F(NodeRegistryTest, RejectedRegistrationLeavesEntriesAndStickyDiagnostic) {
  enum class Defect {
    kNullDefinition,
    kEmptyType,
    kType,
    kCreator,
    kStructure,
    kDuplicate,
    kControl
  };
  struct Case {
    const char* name;
    Defect defect;
    const char* diagnostic;
  };
  const std::vector<Case> cases{
      {"null Definition", Defect::kNullDefinition,
       "requires a valid Definition"},
      {"empty type", Defect::kEmptyType, "Empty node_type"},
      {"wrong type", Defect::kType, "node_type mismatch"},
      {"null creator", Defect::kCreator, "null creator"},
      {"invalid Definition", Defect::kStructure, "Port key cannot be empty"},
      {"duplicate type", Defect::kDuplicate, "Duplicate node registration"},
      {"cross-node Control ID", Defect::kControl,
       "Control ID 77 conflicts between base and candidate"},
  };
  auto& registry = NodeRegistry::Instance();
  for (const auto& row : cases) {
    SCOPED_TRACE(row.name);
    test_support::RegistryTestAccess::ResetNodes();
    ASSERT_TRUE(registry.Register("base", Creator(), Definition("base", 77)));
    const std::string type = row.defect == Defect::kDuplicate   ? "base"
                             : row.defect == Defect::kEmptyType ? ""
                                                                : "candidate";
    auto candidate = Definition(type);
    if (row.defect == Defect::kType) candidate.node_type = "different";
    if (row.defect == Defect::kStructure)
      candidate.inputs[0].logical_name.clear();
    if (row.defect == Defect::kControl)
      candidate.control_commands.emplace_back(77, "another");
    NodeRegistry::CreatorFunc creator = Creator();
    if (row.defect == Defect::kCreator) creator = {};
    EXPECT_FALSE(registry.Register(
        type, std::move(creator),
        row.defect == Defect::kNullDefinition ? nullptr : &candidate));
    EXPECT_TRUE(registry.Find("base").has_value());
    EXPECT_FALSE(registry.Find("candidate").has_value());
    ASSERT_TRUE(registry.HasConflict());
    const auto errors = registry.GetConflictErrors();
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_NE(errors[0].find(row.diagnostic), std::string::npos);
    auto snapshot = registry.Snapshot();
    EXPECT_TRUE(snapshot.has_conflict);
    EXPECT_EQ(snapshot.conflict_errors, errors);
    snapshot.conflict_errors.clear();
    ASSERT_TRUE(registry.Register("later", Creator(), Definition("later")));
    EXPECT_TRUE(registry.HasConflict());
    EXPECT_EQ(registry.GetConflictErrors(), errors);
  }
}

TEST_F(NodeRegistryTest,
       DefinitionFactoryExceptionsBecomeRegistrationFailures) {
  auto& registry = NodeRegistry::Instance();
  for (bool standard : {true, false}) {
    SCOPED_TRACE(standard);
    test_support::RegistryTestAccess::ResetNodes();
    EXPECT_FALSE(registry.RegisterWithDefinitionFactory(
        "factory_probe", Creator(), [=]() -> NodeDefinition {
          if (standard) throw std::runtime_error("literal factory exception");
          throw 42;
        }));
    EXPECT_FALSE(registry.Find("factory_probe").has_value());
    EXPECT_TRUE(registry.HasConflict());
    const auto errors = registry.GetConflictErrors();
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_NE(errors[0].find("factory_probe definition error"),
              std::string::npos);
    EXPECT_NE(errors[0].find(standard ? "literal factory exception"
                                      : "unknown exception"),
              std::string::npos);
  }
}

struct ReentryReads {
  struct Attempt {
    const char* stage;
    std::future<bool> read;
    bool ready_inside;
  };
  std::atomic<bool> armed{false};
  std::vector<Attempt> attempts;
  void Observe(const char* stage) {
    auto read = std::async(std::launch::async, [] {
      auto& registry = NodeRegistry::Instance();
      return registry.Find("reentry").has_value() &&
             registry.ListDefinitions().size() == 1 &&
             registry.Snapshot().definitions.size() == 1;
    });
    // 创建者代码执行期间不要 join：若锁出现回归，被阻塞的读者必须等 Create
    // 完成后才能结束。
    const bool ready =
        read.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    attempts.push_back({stage, std::move(read), ready});
  }
};
struct ReentrantCreator {
  std::shared_ptr<ReentryReads> observations;
  explicit ReentrantCreator(std::shared_ptr<ReentryReads> state)
      : observations(std::move(state)) {}
  ReentrantCreator(const ReentrantCreator& other)
      : observations(other.observations) {
    if (observations->armed.load()) observations->Observe("copy");
  }
  std::unique_ptr<INode> operator()() const {
    observations->Observe("invoke");
    return std::make_unique<RegistryProbeNode>();
  }
};

TEST_F(NodeRegistryTest, CreatorCopyAndInvocationBothPermitRegistryReentry) {
  auto observations = std::make_shared<ReentryReads>();
  auto& registry = NodeRegistry::Instance();
  ASSERT_TRUE(registry.Register("reentry", ReentrantCreator(observations),
                                Definition("reentry")));
  observations->armed.store(true);
  auto creating = std::async(std::launch::async,
                             [&] { return registry.Create("reentry"); });
  const bool ready =
      creating.wait_for(std::chrono::seconds(8)) == std::future_status::ready;
  auto node = creating.get();
  std::vector<bool> results;
  for (auto& attempt : observations->attempts)
    results.push_back(attempt.read.get());
  EXPECT_TRUE(ready);
  EXPECT_NE(node, nullptr);
  ASSERT_EQ(observations->attempts.size(), 2U);
  EXPECT_STREQ(observations->attempts[0].stage, "copy");
  EXPECT_STREQ(observations->attempts[1].stage, "invoke");
  for (size_t i = 0; i < results.size(); ++i) {
    SCOPED_TRACE(observations->attempts[i].stage);
    EXPECT_TRUE(observations->attempts[i].ready_inside);
    EXPECT_TRUE(results[i]);
  }
}

}  // namespace
}  // namespace llm_edgeflow

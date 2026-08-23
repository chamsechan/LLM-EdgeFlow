#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/pipeline_diagnostic.h"
#include "engine/engine_interface.h"
#include "engine/engine_registry.h"

namespace alg_framework {

class DummyNode : public INode {
 public:
  bool Init(const nlohmann::json&, SessionContext*) override { return true; }
  int Process(AlgContext*) override { return 0; }
  int Control(int, const std::string&) override { return 0; }
  const std::string& Name() const override {
    static const std::string name = "DummyNode";
    return name;
  }
};
REGISTER_NODE(DummyNode);

class DummyEngine : public IModelEngine {
 public:
  bool Load(const std::string&, const nlohmann::json&) override { return true; }
  size_t GetMaxBatchSize() const override { return 1; }
  const std::string& EngineType() const override {
    static const std::string type = "dummy_engine";
    return type;
  }
};
REGISTER_ENGINE("dummy_engine", DummyEngine);

// FINAL-R1-002: 独立 TEST 隔离 Node 冲突与 Engine 冲突，杜绝跨套件污染

// 1. 独立验证 Node 注册冲突与 fail-closed
TEST(RegistryConflictNodeTest, DuplicateNodeFailClosed) {
  ASSERT_FALSE(NodeFactory::Instance().HasConflict());
  ASSERT_FALSE(EngineFactory::Instance().HasConflict());

  bool dup_node = NodeFactory::Instance().Register(
      "DummyNode", []() { return std::make_unique<DummyNode>(); });
  EXPECT_FALSE(dup_node);
  EXPECT_TRUE(NodeFactory::Instance().HasConflict());

  // 首次注册必须保留
  auto node_inst = NodeFactory::Instance().Create("DummyNode");
  EXPECT_NE(node_inst, nullptr);

  // Pipeline 构建 fail-closed 并定位至 /pipeline
  Pipeline pipe;
  PipelineDiagnostic diag;
  nlohmann::json cfg = {
      {"business_name", "conflict_node_test"},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_0_DummyNode"},
                               {"node_type", "DummyNode"},
                               {"depends_on", nlohmann::json::array()}}})}};

  EXPECT_FALSE(pipe.BuildFromJson(cfg, &diag));
  EXPECT_EQ(diag.code, PipelineErrorCode::kRegistryConflict);
  EXPECT_EQ(diag.path, "/pipeline");
}

// 2. 独立验证 Engine 注册冲突与 fail-closed
TEST(RegistryConflictEngineTest, DuplicateEngineFailClosed) {
  ASSERT_FALSE(NodeFactory::Instance().HasConflict());
  ASSERT_FALSE(EngineFactory::Instance().HasConflict());

  bool dup_engine = EngineFactory::Instance().Register(
      "dummy_engine", []() { return std::make_unique<DummyEngine>(); });
  EXPECT_FALSE(dup_engine);
  EXPECT_TRUE(EngineFactory::Instance().HasConflict());

  auto engine_inst = EngineFactory::Instance().Create("dummy_engine");
  EXPECT_NE(engine_inst, nullptr);

  // Pipeline 构建 fail-closed 并定位至 /models
  Pipeline pipe;
  PipelineDiagnostic diag;
  nlohmann::json cfg = {
      {"business_name", "conflict_engine_test"},
      {"models", nlohmann::json::array(
                     {{{"model_id", "m1"}, {"engine_type", "dummy_engine"}}})},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_0_DummyNode"},
                               {"node_type", "DummyNode"},
                               {"depends_on", nlohmann::json::array()}}})}};

  EXPECT_FALSE(pipe.BuildFromJson(cfg, &diag));
  EXPECT_EQ(diag.code, PipelineErrorCode::kRegistryConflict);
  EXPECT_EQ(diag.path, "/models");
}

}  // namespace alg_framework

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

// RECHECK-R1-002: 独立进程验证静态注册冲突与 Fail-Closed 行为 (零生产 reset
// 接口)
TEST(RegistryConflictTest, DuplicateNodeAndEngineRegistrationFailClosed) {
  // 1. 验证启动时原始静态注册无冲突
  ASSERT_FALSE(NodeFactory::Instance().HasConflict());
  ASSERT_FALSE(EngineFactory::Instance().HasConflict());

  // 2. 模拟重复 Node 注册
  bool dup_node = NodeFactory::Instance().Register(
      "DummyNode", []() { return std::make_unique<DummyNode>(); });
  EXPECT_FALSE(dup_node);
  EXPECT_TRUE(NodeFactory::Instance().HasConflict());

  // 首次注册必须保留
  auto node_inst = NodeFactory::Instance().Create("DummyNode");
  EXPECT_NE(node_inst, nullptr);

  // 3. 冲突状态下 Pipeline Build 必须 fail-closed
  Pipeline pipe_node;
  PipelineDiagnostic diag;
  nlohmann::json cfg = {
      {"business_name", "conflict_test"},
      {"pipeline", nlohmann::json::array({{{"node_type", "DummyNode"}}})}};

  EXPECT_FALSE(pipe_node.BuildFromJson(cfg, &diag));
  EXPECT_EQ(diag.code, PipelineErrorCode::kRegistryConflict);

  // 4. 模拟重复 Engine 注册
  bool dup_engine = EngineFactory::Instance().Register(
      "dummy_engine", []() { return std::make_unique<DummyEngine>(); });
  EXPECT_FALSE(dup_engine);
  EXPECT_TRUE(EngineFactory::Instance().HasConflict());

  auto engine_inst = EngineFactory::Instance().Create("dummy_engine");
  EXPECT_NE(engine_inst, nullptr);

  Pipeline pipe_engine;
  diag.Clear();
  EXPECT_FALSE(pipe_engine.BuildFromJson(cfg, &diag));
  EXPECT_EQ(diag.code, PipelineErrorCode::kRegistryConflict);
}

}  // namespace alg_framework

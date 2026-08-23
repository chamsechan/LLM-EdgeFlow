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

// RECHECK-R1-003: 构造期重入自身 Registry 查询，测试锁粒度是否正确释放
class ReentrantNode : public INode {
 public:
  ReentrantNode() {
    // 构造期间同步调用 NodeFactory 查询
    volatile bool has = NodeFactory::Instance().Has("ReentrantNode");
    (void)has;
  }
  bool Init(const nlohmann::json&, SessionContext*) override { return true; }
  int Process(AlgContext*) override { return 0; }
  int Control(int, const std::string&) override { return 0; }
  const std::string& Name() const override {
    static const std::string name = "ReentrantNode";
    return name;
  }
};
REGISTER_NODE(ReentrantNode);

class ReentrantEngine : public IModelEngine {
 public:
  ReentrantEngine() {
    // 构造期间同步调用 EngineFactory 查询
    volatile bool has = EngineFactory::Instance().Has("reentrant_engine");
    (void)has;
  }
  bool Load(const std::string&, const nlohmann::json&) override { return true; }
  size_t GetMaxBatchSize() const override { return 1; }
  const std::string& EngineType() const override {
    static const std::string type = "reentrant_engine";
    return type;
  }
};
REGISTER_ENGINE("reentrant_engine", ReentrantEngine);

TEST(RegistryReentrantTest, ReentrantCreationZeroDeadlock) {
  // 1. 同步测试 Node 构造期重入 NodeFactory
  {
    Pipeline p;
    PipelineDiagnostic diag;
    nlohmann::json cfg = {
        {"business_name", "reentrant_node_test"},
        {"pipeline",
         nlohmann::json::array({{{"id", "node_0_ReentrantNode"},
                                 {"node_type", "ReentrantNode"},
                                 {"depends_on", nlohmann::json::array()}}})}};
    EXPECT_TRUE(p.BuildFromJson(cfg, &diag));
    EXPECT_TRUE(p.IsReady());
  }

  // 2. 同步测试 Engine 构造期重入 EngineFactory
  {
    Pipeline p;
    PipelineDiagnostic diag;
    nlohmann::json cfg = {
        {"business_name", "reentrant_engine_test"},
        {"models",
         nlohmann::json::array(
             {{{"model_id", "m1"}, {"engine_type", "reentrant_engine"}}})},
        {"pipeline",
         nlohmann::json::array({{{"id", "node_0_ReentrantNode"},
                                 {"node_type", "ReentrantNode"},
                                 {"depends_on", nlohmann::json::array()}}})}};
    EXPECT_TRUE(p.BuildFromJson(cfg, &diag));
    EXPECT_TRUE(p.IsReady());
  }
}

}  // namespace alg_framework

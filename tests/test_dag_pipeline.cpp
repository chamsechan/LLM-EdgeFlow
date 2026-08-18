#include <gtest/gtest.h>

#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/alg_context.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/pipeline.h"

namespace alg_framework {

// 辅助测试算子定义
class DagTestNodeA : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    (void)config;
    (void)session_ctx;
    return true;
  }
  int Process(AlgContext* req_ctx) override {
    auto* trace = req_ctx->Get<std::vector<std::string>>("exec_trace");
    if (trace) {
      trace->push_back("NodeA");
    }
    req_ctx->Set("node_a_out", std::string("DataFromA"));
    return 0;
  }
  int Control(int cmd, const std::string& param) override {
    (void)cmd;
    (void)param;
    return 0;
  }
  const std::string& Name() const override {
    static const std::string name = "DagTestNodeA";
    return name;
  }
};
REGISTER_NODE(DagTestNodeA);

class DagTestNodeB : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    (void)config;
    (void)session_ctx;
    return true;
  }
  int Process(AlgContext* req_ctx) override {
    auto* trace = req_ctx->Get<std::vector<std::string>>("exec_trace");
    if (trace) {
      trace->push_back("NodeB");
    }
    // 必须依赖 NodeA 的输出
    auto* a_out = req_ctx->Get<std::string>("node_a_out");
    if (!a_out) return -101;
    req_ctx->Set("node_b_out", std::string("DataFromB_after_") + *a_out);
    return 0;
  }
  int Control(int cmd, const std::string& param) override {
    (void)cmd;
    (void)param;
    return 0;
  }
  const std::string& Name() const override {
    static const std::string name = "DagTestNodeB";
    return name;
  }
};
REGISTER_NODE(DagTestNodeB);

class DagTestNodeC : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    (void)config;
    (void)session_ctx;
    return true;
  }
  int Process(AlgContext* req_ctx) override {
    auto* trace = req_ctx->Get<std::vector<std::string>>("exec_trace");
    if (trace) {
      trace->push_back("NodeC");
    }
    // 依赖 NodeA 的输出 (分支 2)
    auto* a_out = req_ctx->Get<std::string>("node_a_out");
    if (!a_out) return -102;
    req_ctx->Set("node_c_out", std::string("DataFromC_after_") + *a_out);
    return 0;
  }
  int Control(int cmd, const std::string& param) override {
    (void)cmd;
    (void)param;
    return 0;
  }
  const std::string& Name() const override {
    static const std::string name = "DagTestNodeC";
    return name;
  }
};
REGISTER_NODE(DagTestNodeC);

class DagTestNodeD : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    (void)config;
    (void)session_ctx;
    return true;
  }
  int Process(AlgContext* req_ctx) override {
    auto* trace = req_ctx->Get<std::vector<std::string>>("exec_trace");
    if (trace) {
      trace->push_back("NodeD");
    }
    // 汇聚 NodeB 和 NodeC 两个分支
    auto* b_out = req_ctx->Get<std::string>("node_b_out");
    auto* c_out = req_ctx->Get<std::string>("node_c_out");
    if (!b_out || !c_out) return -103;

    req_ctx->Set("final_dag_result", *b_out + " + " + *c_out);
    return 0;
  }
  int Control(int cmd, const std::string& param) override {
    (void)cmd;
    (void)param;
    return 0;
  }
  const std::string& Name() const override {
    static const std::string name = "DagTestNodeD";
    return name;
  }
};
REGISTER_NODE(DagTestNodeD);

// -----------------------------------------------------------------------------
// GTest 测试套件
// -----------------------------------------------------------------------------
class DagPipelineTest : public ::testing::Test {};

// 1. 乱序书写自动拓扑重排 (Shuffled JSON -> Correct Order)
TEST_F(DagPipelineTest, ShuffledOrderTopologicalSort) {
  // JSON 中故意将 D 写在最前，B 和 C 其次，A 写在最后 (逆序输入)
  nlohmann::json config = {{"business_name", "shuffled_dag_test"},
                           {"pipeline",
                            {{{"id", "node_d"},
                              {"node_type", "DagTestNodeD"},
                              {"depends_on", {"node_b", "node_c"}}},
                             {{"id", "node_c"},
                              {"node_type", "DagTestNodeC"},
                              {"depends_on", {"node_a"}}},
                             {{"id", "node_b"},
                              {"node_type", "DagTestNodeB"},
                              {"depends_on", {"node_a"}}},
                             {{"id", "node_a"},
                              {"node_type", "DagTestNodeA"},
                              {"depends_on", nlohmann::json::array()}}}}};

  Pipeline pipeline;
  bool ok = pipeline.BuildFromJson(config);
  ASSERT_TRUE(ok);

  // 校验拓扑序：node_a 必须在第一位，node_d 必须在最后一位
  const auto& order = pipeline.GetTopologicalOrder();
  ASSERT_EQ(order.size(), 4);
  EXPECT_EQ(order[0], "node_a");
  EXPECT_EQ(order[3], "node_d");

  // 执行管线并验证执行轨迹
  AlgContext req_ctx;
  req_ctx.Set("exec_trace", std::vector<std::string>{});

  int ret = pipeline.Execute(&req_ctx);
  EXPECT_EQ(ret, 0);

  auto* trace = req_ctx.Get<std::vector<std::string>>("exec_trace");
  ASSERT_NE(trace, nullptr);
  ASSERT_EQ(trace->size(), 4);
  EXPECT_EQ((*trace)[0], "NodeA");
  EXPECT_EQ((*trace)[3], "NodeD");

  auto* final_res = req_ctx.Get<std::string>("final_dag_result");
  ASSERT_NE(final_res, nullptr);
  EXPECT_EQ(*final_res,
            "DataFromB_after_DataFromA + DataFromC_after_DataFromA");
}

// 2. 钻石分支与汇聚拓扑测试 (Diamond Branch & Merge)
TEST_F(DagPipelineTest, DiamondBranchAndMerge) {
  nlohmann::json config = {
      {"business_name", "diamond_dag_test"},
      {"pipeline",
       {{{"id", "A"},
         {"node_type", "DagTestNodeA"},
         {"depends_on", nlohmann::json::array()}},
        {{"id", "B"}, {"node_type", "DagTestNodeB"}, {"depends_on", {"A"}}},
        {{"id", "C"}, {"node_type", "DagTestNodeC"}, {"depends_on", {"A"}}},
        {{"id", "D"},
         {"node_type", "DagTestNodeD"},
         {"depends_on", {"B", "C"}}}}}};

  Pipeline pipeline;
  ASSERT_TRUE(pipeline.BuildFromJson(config));

  AlgContext req_ctx;
  req_ctx.Set("exec_trace", std::vector<std::string>{});

  int ret = pipeline.Execute(&req_ctx);
  EXPECT_EQ(ret, 0);

  auto* trace = req_ctx.Get<std::vector<std::string>>("exec_trace");
  ASSERT_NE(trace, nullptr);
  ASSERT_EQ(trace->size(), 4);
  EXPECT_EQ((*trace)[0], "NodeA");
  EXPECT_EQ((*trace)[3], "NodeD");
}

// 3. 循环依赖死锁检测 (Cycle Detection: A -> B -> C -> A)
TEST_F(DagPipelineTest, CycleDetectionRejection) {
  nlohmann::json cyclic_config = {
      {"business_name", "cyclic_pipeline"},
      {"pipeline",
       {
           {{"id", "A"},
            {"node_type", "DagTestNodeA"},
            {"depends_on", {"C"}}},  // A 依赖 C
           {{"id", "B"},
            {"node_type", "DagTestNodeB"},
            {"depends_on", {"A"}}},  // B 依赖 A
           {{"id", "C"},
            {"node_type", "DagTestNodeC"},
            {"depends_on", {"B"}}}  // C 依赖 B (构成闭环)
       }}};

  Pipeline pipeline;
  bool ok = pipeline.BuildFromJson(cyclic_config);
  // 必须拦截成环并返回 false，禁止启动
  EXPECT_FALSE(ok);
}

// 4. 自环死锁检测 (Self Loop: A -> A)
TEST_F(DagPipelineTest, SelfLoopCycleRejection) {
  nlohmann::json self_loop_config = {
      {"business_name", "self_loop_pipeline"},
      {"pipeline",
       {{{"id", "A"}, {"node_type", "DagTestNodeA"}, {"depends_on", {"A"}}}}}};

  Pipeline pipeline;
  EXPECT_FALSE(pipeline.BuildFromJson(self_loop_config));
}

// 5. 非法依赖 ID 校验 (Non-existent Dependency ID)
TEST_F(DagPipelineTest, InvalidDependencyRejection) {
  nlohmann::json invalid_dep_config = {
      {"business_name", "invalid_dep_pipeline"},
      {"pipeline",
       {{{"id", "A"},
         {"node_type", "DagTestNodeA"},
         {"depends_on", {"ghost_non_existent_node"}}}}}};

  Pipeline pipeline;
  EXPECT_FALSE(pipeline.BuildFromJson(invalid_dep_config));
}

// 6. 存量线性配置向后兼容性 (100% Backward Compatibility)
TEST_F(DagPipelineTest, BackwardCompatibilityWithoutDependsOn) {
  nlohmann::json legacy_config = {{"business_name", "legacy_linear_pipeline"},
                                  {"pipeline",
                                   {{{"node_type", "DagTestNodeA"}},
                                    {{"node_type", "DagTestNodeB"}},
                                    {{"node_type", "DagTestNodeC"}},
                                    {{"node_type", "DagTestNodeD"}}}}};

  Pipeline pipeline;
  ASSERT_TRUE(pipeline.BuildFromJson(legacy_config));

  AlgContext req_ctx;
  req_ctx.Set("exec_trace", std::vector<std::string>{});

  int ret = pipeline.Execute(&req_ctx);
  EXPECT_EQ(ret, 0);

  auto* trace = req_ctx.Get<std::vector<std::string>>("exec_trace");
  ASSERT_NE(trace, nullptr);
  ASSERT_EQ(trace->size(), 4);
  EXPECT_EQ((*trace)[0], "NodeA");
  EXPECT_EQ((*trace)[1], "NodeB");
  EXPECT_EQ((*trace)[2], "NodeC");
  EXPECT_EQ((*trace)[3], "NodeD");
}

}  // namespace alg_framework

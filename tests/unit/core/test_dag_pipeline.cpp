#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/alg_context.h"
#include "core/node_interface.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "tests/support/pipeline_test_utils.h"

namespace llm_edgeflow {

static std::mutex s_trace_mutex;
static std::vector<std::string> s_execution_trace;

static void ResetExecutionTrace() {
  std::lock_guard<std::mutex> lock(s_trace_mutex);
  s_execution_trace.clear();
}

static void AppendExecutionTrace(std::string node_name) {
  std::lock_guard<std::mutex> lock(s_trace_mutex);
  s_execution_trace.push_back(std::move(node_name));
}

static std::vector<std::string> SnapshotExecutionTrace() {
  std::lock_guard<std::mutex> lock(s_trace_mutex);
  return s_execution_trace;
}

inline NodeDefinition MakeDagNodeDef(const std::string& type,
                                     std::vector<NodePortDefinition> inputs,
                                     std::vector<NodePortDefinition> outputs) {
  NodeDefinition def;
  def.node_type = type;
  def.category = "test";
  def.description = "test dag node";
  def.inputs = std::move(inputs);
  def.outputs = std::move(outputs);
  def.parallel_safe = true;
  return def;
}

// 辅助测试算子定义
class DagTestNodeA : public INode {
 public:
  inline static constexpr char kNodeType[] = "dag_test_node_a";
  bool Init(const NodeInitContext& init_ctx) override {
    plan_ = init_ctx.plan;
    return plan_ != nullptr;
  }
  int Process(AlgContext* req_ctx) override {
    AppendExecutionTrace("NodeA");
    if (const auto* output =
            plan_->FindPort("node_a_out", PortDirection::kOutput))
      req_ctx->Publish(output->blackboard_key, std::string("DataFromA"));
    return 0;
  }
  NodeControlResult Control(int cmd, const std::string& param) override {
    (void)cmd;
    (void)param;
    return NodeControlResult::Handled(0);
  }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }

 private:
  const ValidatedNodePlan* plan_ = nullptr;
};
REGISTER_NODE_WITH_DEFINITION(DagTestNodeA,
                              MakeDagNodeDef(DagTestNodeA::kNodeType, {},
                                             {{"node_a_out", "string"}}));

class DagTestNodeB : public INode {
 public:
  inline static constexpr char kNodeType[] = "dag_test_node_b";
  bool Init(const NodeInitContext& init_ctx) override {
    plan_ = init_ctx.plan;
    return plan_ != nullptr;
  }
  int Process(AlgContext* req_ctx) override {
    AppendExecutionTrace("NodeB");
    // 必须依赖 NodeA 的输出
    auto* a_out = req_ctx->Read<std::string>(
        plan_->FindPort("node_a_out")->blackboard_key);
    if (!a_out) return -101;
    if (const auto* output =
            plan_->FindPort("node_b_out", PortDirection::kOutput))
      req_ctx->Publish(output->blackboard_key,
                       std::string("DataFromB_after_") + *a_out);
    return 0;
  }
  NodeControlResult Control(int cmd, const std::string& param) override {
    (void)cmd;
    (void)param;
    return NodeControlResult::Handled(0);
  }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }

 private:
  const ValidatedNodePlan* plan_ = nullptr;
};
REGISTER_NODE_WITH_DEFINITION(DagTestNodeB,
                              MakeDagNodeDef(DagTestNodeB::kNodeType,
                                             {{"node_a_out", "string"}},
                                             {{"node_b_out", "string"}}));

class DagTestNodeC : public INode {
 public:
  inline static constexpr char kNodeType[] = "dag_test_node_c";
  bool Init(const NodeInitContext& init_ctx) override {
    plan_ = init_ctx.plan;
    return plan_ != nullptr;
  }
  int Process(AlgContext* req_ctx) override {
    AppendExecutionTrace("NodeC");
    // 依赖 NodeA 的输出 (分支 2)
    auto* a_out = req_ctx->Read<std::string>(
        plan_->FindPort("node_a_out")->blackboard_key);
    if (!a_out) return -102;
    if (const auto* output =
            plan_->FindPort("node_c_out", PortDirection::kOutput))
      req_ctx->Publish(output->blackboard_key,
                       std::string("DataFromC_after_") + *a_out);
    return 0;
  }
  NodeControlResult Control(int cmd, const std::string& param) override {
    (void)cmd;
    (void)param;
    return NodeControlResult::Handled(0);
  }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }

 private:
  const ValidatedNodePlan* plan_ = nullptr;
};
REGISTER_NODE_WITH_DEFINITION(DagTestNodeC,
                              MakeDagNodeDef(DagTestNodeC::kNodeType,
                                             {{"node_a_out", "string"}},
                                             {{"node_c_out", "string"}}));

class DagTestUnsafeNodeC : public DagTestNodeC {
 public:
  inline static constexpr char kNodeType[] = "dag_test_unsafe_node_c";
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};

inline NodeDefinition MakeUnsafeDagNodeDef() {
  auto def =
      MakeDagNodeDef(DagTestUnsafeNodeC::kNodeType, {{"node_a_out", "string"}},
                     {{"node_c_out", "string"}});
  def.parallel_safe = false;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(DagTestUnsafeNodeC, MakeUnsafeDagNodeDef());

class DagTestNodeD : public INode {
 public:
  inline static constexpr char kNodeType[] = "dag_test_node_d";
  bool Init(const NodeInitContext& init_ctx) override {
    plan_ = init_ctx.plan;
    return plan_ != nullptr;
  }
  int Process(AlgContext* req_ctx) override {
    AppendExecutionTrace("NodeD");
    // 汇聚 NodeB 和 NodeC 两个分支
    auto* b_out = req_ctx->Read<std::string>(
        plan_->FindPort("node_b_out")->blackboard_key);
    auto* c_out = req_ctx->Read<std::string>(
        plan_->FindPort("node_c_out")->blackboard_key);
    if (!b_out || !c_out) return -103;

    if (const auto* output =
            plan_->FindPort("final_dag_result", PortDirection::kOutput))
      req_ctx->Publish(output->blackboard_key, *b_out + " + " + *c_out);
    return 0;
  }
  NodeControlResult Control(int cmd, const std::string& param) override {
    (void)cmd;
    (void)param;
    return NodeControlResult::Handled(0);
  }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }

 private:
  const ValidatedNodePlan* plan_ = nullptr;
};
REGISTER_NODE_WITH_DEFINITION(DagTestNodeD,
                              MakeDagNodeDef(DagTestNodeD::kNodeType,
                                             {{"node_b_out", "string"},
                                              {"node_c_out", "string"}},
                                             {{"final_dag_result", "string"}}));

class ThrowingProcessDagNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "throwing_process_dag";
  bool Init(const NodeInitContext&) override { return true; }
  static inline int failure_mode = 0;
  int Process(AlgContext* ctx) override {
    if (failure_mode == 1) throw 42;
    if (failure_mode == 2) return -8103;
    if (failure_mode == 3 || failure_mode == 4) {
      ctx->SetError(-9999, "diagnostic from this invocation");
      return -8104;
    }
    throw std::runtime_error("parallel process failure");
  }
  const std::string& Name() const override {
    if (failure_mode == 4) throw std::bad_alloc();
    static const std::string name = kNodeType;
    return name;
  }
};
REGISTER_NODE_WITH_DEFINITION(ThrowingProcessDagNode,
                              MakeDagNodeDef(ThrowingProcessDagNode::kNodeType,
                                             {}, {}));

class GatedProcessDagNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "gated_process_dag";

  static void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
    released_ = false;
    completed_.store(false);
  }

  static bool WaitUntilStarted(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, []() { return started_; });
  }

  static void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    condition_.notify_all();
  }

  static bool Completed() { return completed_.load(); }

  bool Init(const NodeInitContext&) override { return true; }
  int Process(AlgContext*) override {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      started_ = true;
      condition_.notify_all();
      condition_.wait(lock, []() { return released_; });
    }
    completed_.store(true);
    return 0;
  }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }

 private:
  static inline std::mutex mutex_;
  static inline std::condition_variable condition_;
  static inline bool started_ = false;
  static inline bool released_ = false;
  static inline std::atomic<bool> completed_{false};
};
REGISTER_NODE_WITH_DEFINITION(GatedProcessDagNode,
                              MakeDagNodeDef(GatedProcessDagNode::kNodeType, {},
                                             {}));

class ParallelFailureCoordinator {
 public:
  static void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    first_error_set_ = false;
    second_error_set_ = false;
  }

  static void MarkFirstAndWaitForSecond() {
    std::unique_lock<std::mutex> lock(mutex_);
    first_error_set_ = true;
    condition_.notify_all();
    condition_.wait(lock, []() { return second_error_set_; });
  }

  static void WaitForFirst() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, []() { return first_error_set_; });
  }

  static void MarkSecond() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      second_error_set_ = true;
    }
    condition_.notify_all();
  }

 private:
  static inline std::mutex mutex_;
  static inline std::condition_variable condition_;
  static inline bool first_error_set_ = false;
  static inline bool second_error_set_ = false;
};

class FirstFailingDagNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "first_failing_dag";
  bool Init(const NodeInitContext&) override { return true; }
  int Process(AlgContext* req_ctx) override {
    req_ctx->SetError(-8101, "first parallel failure");
    ParallelFailureCoordinator::MarkFirstAndWaitForSecond();
    return -8101;
  }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};
REGISTER_NODE_WITH_DEFINITION(FirstFailingDagNode,
                              MakeDagNodeDef(FirstFailingDagNode::kNodeType, {},
                                             {}));

class SecondFailingDagNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "second_failing_dag";
  bool Init(const NodeInitContext&) override { return true; }
  int Process(AlgContext* req_ctx) override {
    ParallelFailureCoordinator::WaitForFirst();
    req_ctx->SetError(-8102, "second parallel failure");
    ParallelFailureCoordinator::MarkSecond();
    return -8102;
  }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};
REGISTER_NODE_WITH_DEFINITION(SecondFailingDagNode,
                              MakeDagNodeDef(SecondFailingDagNode::kNodeType,
                                             {}, {}));

// -----------------------------------------------------------------------------
// GTest 测试套件
// -----------------------------------------------------------------------------
class DagPipelineTest : public ::testing::Test {};

// 1. 乱序书写自动拓扑重排 (Shuffled JSON -> Correct Order)
TEST_F(DagPipelineTest, ShuffledOrderTopologicalSort) {
  // JSON 中故意将 D 写在最前，B 和 C 其次，A 写在最后 (逆序输入)
  nlohmann::json config = {
      {"pipeline",
       {{{"name", "node_d"},
         {"type", "dag_test_node_d"},
         {"inputs",
          {{"node_b_out", "node_b.node_b_out"},
           {"node_c_out", "node_c.node_c_out"}}}},
        {{"name", "node_c"},
         {"type", "dag_test_node_c"},
         {"inputs", {{"node_a_out", "node_a.node_a_out"}}}},
        {{"name", "node_b"},
         {"type", "dag_test_node_b"},
         {"inputs", {{"node_a_out", "node_a.node_a_out"}}}},
        {{"name", "node_a"}, {"type", "dag_test_node_a"}}}}};

  for (int workers : {1, 4}) {
    SCOPED_TRACE(workers);
    config["max_parallel_workers"] = workers;
    Pipeline pipeline;
    bool ok = BuildTestPipeline(
        pipeline, config,
        MakeTestBoundary({}, {{"node_d.final_dag_result", "string"}}), nullptr);
    ASSERT_TRUE(ok);

    // 校验拓扑序：node_a 必须在第一位，node_d 必须在最后一位
    const auto& order = pipeline.GetTopologicalOrder();
    ASSERT_EQ(order.size(), 4U);
    EXPECT_EQ(order[0], "node_a");
    EXPECT_EQ(order[3], "node_d");

    // 执行管线并验证执行轨迹
    AlgContext req_ctx;
    ResetExecutionTrace();

    int ret = pipeline.Execute(&req_ctx);
    EXPECT_EQ(ret, 0);

    const auto trace = SnapshotExecutionTrace();
    ASSERT_EQ(trace.size(), 4);
    EXPECT_EQ(trace[0], "NodeA");
    EXPECT_EQ(trace[3], "NodeD");

    auto* final_res = req_ctx.Read<std::string>("node_d.final_dag_result");
    ASSERT_NE(final_res, nullptr);
    EXPECT_EQ(*final_res,
              "DataFromB_after_DataFromA + DataFromC_after_DataFromA");
  }
}

// 2. 钻石分支与汇聚拓扑测试 (Diamond Branch & Merge)
TEST_F(DagPipelineTest, DiamondBranchAndMerge) {
  nlohmann::json config = {
      {"pipeline",
       {{{"name", "A"},
         {"type", "dag_test_node_a"},
         {"depends_on", nlohmann::json::array()}},
        {{"name", "B"},
         {"type", "dag_test_node_b"},
         {"inputs", {{"node_a_out", "A.node_a_out"}}},
         {"depends_on", {"A"}}},
        {{"name", "C"},
         {"type", "dag_test_node_c"},
         {"inputs", {{"node_a_out", "A.node_a_out"}}},
         {"depends_on", {"A"}}},
        {{"name", "D"},
         {"type", "dag_test_node_d"},
         {"inputs",
          {{"node_b_out", "B.node_b_out"}, {"node_c_out", "C.node_c_out"}}},
         {"depends_on", {"B", "C"}}}}}};

  Pipeline pipeline;
  ASSERT_TRUE(BuildTestPipeline(pipeline, config, MakeTestBoundary(), nullptr));

  AlgContext req_ctx;
  ResetExecutionTrace();

  int ret = pipeline.Execute(&req_ctx);
  EXPECT_EQ(ret, 0);

  const auto trace = SnapshotExecutionTrace();
  ASSERT_EQ(trace.size(), 4);
  EXPECT_EQ(trace[0], "NodeA");
  EXPECT_EQ(trace[3], "NodeD");
}

// 异步波前分层并发调度测试 (Parallel Wavefront Execution)
TEST_F(DagPipelineTest, ParallelWavefrontExecution) {
  nlohmann::json parallel_config = {
      {"max_parallel_workers", 4},
      {"pipeline",
       {// Layer 0: Root 节点 A
        {{"name", "node_a"},
         {"type", "dag_test_node_a"},
         {"depends_on", nlohmann::json::array()}},
        // Layer 1: 兄弟节点 B 和 C 均依赖 A，在 Layer 1 并发执行
        {{"name", "node_b"},
         {"type", "dag_test_node_b"},
         {"inputs", {{"node_a_out", "node_a.node_a_out"}}},
         {"depends_on", {"node_a"}}},
        {{"name", "node_c"},
         {"type", "dag_test_node_c"},
         {"inputs", {{"node_a_out", "node_a.node_a_out"}}},
         {"depends_on", {"node_a"}}},
        // Layer 2: 汇聚节点 D，依赖 B 和 C
        {{"name", "node_d"},
         {"type", "dag_test_node_d"},
         {"inputs",
          {{"node_b_out", "node_b.node_b_out"},
           {"node_c_out", "node_c.node_c_out"}}},
         {"depends_on", {"node_b", "node_c"}}}}}};

  Pipeline pipeline;
  ASSERT_TRUE(BuildTestPipeline(
      pipeline, parallel_config,
      MakeTestBoundary({}, {{"node_d.final_dag_result", "string"}}), nullptr));
  EXPECT_EQ(pipeline.GetExecutionMode(), Pipeline::ExecutionMode::kParallel);

  const auto& layers = pipeline.GetTopologicalLayers();
  ASSERT_EQ(layers.size(), 3);
  EXPECT_EQ(layers[0].size(), 1);  // 第 0 层：node_a
  EXPECT_EQ(layers[1].size(),
            2);                    // 第 1 层：node_b、node_c (并行波前)
  EXPECT_EQ(layers[2].size(), 1);  // 第 2 层：node_d

  AlgContext req_ctx;
  ResetExecutionTrace();

  int ret = pipeline.Execute(&req_ctx);
  EXPECT_EQ(ret, 0);

  auto* final_res = req_ctx.Read<std::string>("node_d.final_dag_result");
  ASSERT_NE(final_res, nullptr);
  EXPECT_EQ(*final_res,
            "DataFromB_after_DataFromA + DataFromC_after_DataFromA");
}

TEST_F(DagPipelineTest, UnsafeNodeRunsInOwnLayer) {
  nlohmann::json config = {
      {"pipeline",
       {{{"name", "node_a"}, {"type", DagTestNodeA::kNodeType}},
        {{"name", "node_b"},
         {"type", DagTestNodeB::kNodeType},
         {"inputs", {{"node_a_out", "node_a.node_a_out"}}}},
        {{"name", "node_c"},
         {"type", DagTestUnsafeNodeC::kNodeType},
         {"inputs", {{"node_a_out", "node_a.node_a_out"}}}},
        {{"name", "node_d"},
         {"type", DagTestNodeD::kNodeType},
         {"inputs",
          {{"node_b_out", "node_b.node_b_out"},
           {"node_c_out", "node_c.node_c_out"}}}}}}};

  for (int workers : {4, 1}) {
    SCOPED_TRACE(workers);
    config["max_parallel_workers"] = workers;
    const auto report = PipelineValidator::Validate(config, MakeTestBoundary());
    ASSERT_TRUE(report.ok) << report.ToJson().dump(2);
    const std::vector<std::vector<std::string>> expected_layers =
        workers > 1 ? std::vector<std::vector<std::string>>{{"node_a"},
                                                            {"node_b"},
                                                            {"node_c"},
                                                            {"node_d"}}
                    : std::vector<std::vector<std::string>>{
                          {"node_a"}, {"node_b", "node_c"}, {"node_d"}};
    EXPECT_EQ(report.topological_layers, expected_layers);
    EXPECT_EQ(
        report.topological_order,
        (std::vector<std::string>{"node_a", "node_b", "node_c", "node_d"}));

    Pipeline pipeline;
    ASSERT_TRUE(BuildTestPipeline(
        pipeline, config,
        MakeTestBoundary({}, {{"node_d.final_dag_result", "string"}}),
        nullptr));
    AlgContext context;
    ResetExecutionTrace();
    EXPECT_EQ(pipeline.Execute(&context), 0);
    const auto* result = context.Read<std::string>("node_d.final_dag_result");
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(*result, "DataFromB_after_DataFromA + DataFromC_after_DataFromA");
    EXPECT_EQ(SnapshotExecutionTrace(),
              (std::vector<std::string>{"NodeA", "NodeB", "NodeC", "NodeD"}));
  }
}

TEST_F(DagPipelineTest, ParallelExceptionWaitsForAllSubmittedNodes) {
  const nlohmann::json config = {
      {"max_parallel_workers", 2},
      {"pipeline",
       nlohmann::json::array({{{"name", "throwing"},
                               {"type", ThrowingProcessDagNode::kNodeType},
                               {"depends_on", nlohmann::json::array()}},
                              {{"name", "gated"},
                               {"type", GatedProcessDagNode::kNodeType},
                               {"depends_on", nlohmann::json::array()}}})}};

  GatedProcessDagNode::Reset();
  Pipeline pipeline;
  ASSERT_TRUE(BuildTestPipeline(pipeline, config, MakeTestBoundary(), nullptr));

  AlgContext context;
  auto execution = std::async(std::launch::async,
                              [&]() { return pipeline.Execute(&context); });
  EXPECT_TRUE(GatedProcessDagNode::WaitUntilStarted(std::chrono::seconds(1)));
  EXPECT_EQ(execution.wait_for(std::chrono::milliseconds(50)),
            std::future_status::timeout);

  GatedProcessDagNode::Release();
  EXPECT_EQ(execution.get(), -1);
  EXPECT_TRUE(GatedProcessDagNode::Completed());
  EXPECT_FALSE(context.IsOk());
  EXPECT_NE(context.GetErrorMessage().find("parallel process failure"),
            std::string::npos);
}

TEST_F(DagPipelineTest, ParallelFailuresKeepCodeAndMessageFromSameNode) {
  const nlohmann::json config = {
      {"max_parallel_workers", 2},
      {"pipeline",
       nlohmann::json::array({{{"name", "first"},
                               {"type", FirstFailingDagNode::kNodeType},
                               {"depends_on", nlohmann::json::array()}},
                              {{"name", "second"},
                               {"type", SecondFailingDagNode::kNodeType},
                               {"depends_on", nlohmann::json::array()}}})}};

  ParallelFailureCoordinator::Reset();
  Pipeline pipeline;
  ASSERT_TRUE(BuildTestPipeline(pipeline, config, MakeTestBoundary(), nullptr));

  AlgContext context;
  EXPECT_EQ(pipeline.Execute(&context), -8101);
  EXPECT_EQ(context.GetErrorCode(), -8101);
  EXPECT_NE(context.GetErrorMessage().find("Node 'first'"), std::string::npos);
  EXPECT_NE(context.GetErrorMessage().find("first parallel failure"),
            std::string::npos);
}

// 黑板高并发读写线程安全性压测 (Thread-Safe AlgContext Stress Test)
TEST_F(DagPipelineTest, ThreadSafeAlgContextStressTest) {
  AlgContext req_ctx;
  const int num_threads = 16;
  const int ops_per_thread = 500;

  std::vector<std::thread> workers;
  workers.reserve(num_threads);

  for (int t = 0; t < num_threads; ++t) {
    workers.emplace_back([&req_ctx, t]() {
      for (int i = 0; i < ops_per_thread; ++i) {
        std::string my_key =
            "thread_" + std::to_string(t) + "_key_" + std::to_string(i % 10);
        req_ctx.Publish(my_key, i * 100 + t);

        // 并发读取
        const int* val = req_ctx.Read<int>(my_key);
        if (val) {
          EXPECT_GE(*val, 0);
        }

        // 并发交叉读取共享 Key
        if (req_ctx.Has("shared_counter")) {
          req_ctx.Read<int>("shared_counter");
        } else {
          req_ctx.Publish("shared_counter", 1);
        }
      }
    });
  }

  for (auto& w : workers) {
    w.join();
  }

  EXPECT_TRUE(req_ctx.IsOk());
}

TEST_F(DagPipelineTest, SequentialAndSingleNodeParallelShareFailureContract) {
  for (int workers : {1, 4}) {
    for (int failure = 0; failure < 4; ++failure) {
      ThrowingProcessDagNode::failure_mode = failure;
      nlohmann::json config = {{"max_parallel_workers", workers},
                               {"pipeline",
                                {{{"name", "failing"},
                                  {"type", ThrowingProcessDagNode::kNodeType},
                                  {"depends_on", nlohmann::json::array()}}}}};
      Pipeline pipeline;
      EXPECT_TRUE(
          BuildTestPipeline(pipeline, config, MakeTestBoundary(), nullptr));
      AlgContext ctx;
      ctx.SetError(-9998, "stale diagnostic");
      const int expected = failure < 2 ? -1 : (failure == 2 ? -8103 : -8104);
      EXPECT_EQ(pipeline.Execute(&ctx), expected);
      EXPECT_EQ(ctx.GetErrorCode(), expected);
      EXPECT_NE(ctx.GetErrorMessage().find("Node 'failing'"),
                std::string::npos);
      EXPECT_EQ(ctx.GetErrorMessage().find("stale diagnostic"),
                std::string::npos);
      if (failure == 3) {
        EXPECT_NE(ctx.GetErrorMessage().find("diagnostic from this invocation"),
                  std::string::npos);
      }
    }
  }
  ThrowingProcessDagNode::failure_mode = 0;
}

TEST_F(DagPipelineTest, DiagnosticFailureStillWaitsForSubmittedNodes) {
  const nlohmann::json config = {{"max_parallel_workers", 2},
                                 {"pipeline",
                                  {{{"name", "failing"},
                                    {"type", ThrowingProcessDagNode::kNodeType},
                                    {"depends_on", nlohmann::json::array()}},
                                   {{"name", "gated"},
                                    {"type", GatedProcessDagNode::kNodeType},
                                    {"depends_on", nlohmann::json::array()}}}}};
  GatedProcessDagNode::Reset();
  Pipeline pipeline;
  ASSERT_TRUE(BuildTestPipeline(pipeline, config, MakeTestBoundary(), nullptr));
  ThrowingProcessDagNode::failure_mode = 4;
  AlgContext ctx;
  auto execution = std::async(std::launch::async, [&] {
    try {
      return pipeline.Execute(&ctx);
    } catch (const std::bad_alloc&) {
      return -999;
    }
  });
  EXPECT_TRUE(GatedProcessDagNode::WaitUntilStarted(std::chrono::seconds(1)));
  EXPECT_EQ(execution.wait_for(std::chrono::milliseconds(50)),
            std::future_status::timeout);
  GatedProcessDagNode::Release();
  EXPECT_EQ(execution.get(), -999);
  EXPECT_TRUE(GatedProcessDagNode::Completed());
  ThrowingProcessDagNode::failure_mode = 0;
}

}  // namespace llm_edgeflow

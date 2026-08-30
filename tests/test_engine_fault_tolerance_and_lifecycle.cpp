#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "company_alg_cpp.hpp"
#include "company_alg_interface.h"
#include "core/alg_context.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "engine/fixed_batch_executor.h"
#include "engine/model_interface.h"

static std::string GetConfigPath(const std::string& rel_path) {
  FILE* fp = fopen(rel_path.c_str(), "r");
  if (fp) {
    fclose(fp);
    return rel_path;
  }
  return "../" + rel_path;
}

namespace alg_framework {

// 1. 模拟硬件故障的推理引擎 (可动态注入硬件故障)
class MockFaultyHardwareModel : public IEmbeddingModel {
 public:
  const std::string& ModelType() const noexcept override {
    static const std::string type = "mock_faulty_model";
    return type;
  }
  const std::string& Capability() const noexcept override {
    static const std::string capability = "embedding";
    return capability;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  size_t GetMaxBatchSize() const noexcept override { return 4; }

  int Embed(const TextBatch& input_texts, const EmbeddingOptions&,
            EmbeddingBatch* output_embeddings) noexcept override {
    if (should_fail_) {
      // 模拟底层硬件 NPU DMA 超时或驱动错误
      return error_code_to_return_;
    }

    return FixedBatchExecutor::Execute<std::string, std::vector<float>>(
        input_texts, BatchPolicy{GetMaxBatchSize(), GetMaxBatchSize()},
        [](const BatchSlice& slice,
           std::vector<std::vector<float>>* batch_out) -> int {
          batch_out->assign(slice.execution_count,
                            std::vector<float>(128, 0.5f));
          return 0;
        },
        output_embeddings);
  }

  void SetFault(bool fail, int err_code = -777) {
    should_fail_ = fail;
    error_code_to_return_ = err_code;
  }

 private:
  bool should_fail_ = false;
  int error_code_to_return_ = -777;
};

// 2. 析构追踪探针 (用于验证 AlgContext 内存释放与 RAII 确定性)
static std::atomic<int> g_probe_constructed_count{0};
static std::atomic<int> g_probe_destructed_count{0};

struct LifetimeProbe {
  std::vector<float> large_payload;
  explicit LifetimeProbe(size_t elements = 10000)
      : large_payload(elements, 1.234f) {
    g_probe_constructed_count.fetch_add(1);
  }
  LifetimeProbe(const LifetimeProbe& o) : large_payload(o.large_payload) {
    g_probe_constructed_count.fetch_add(1);
  }
  LifetimeProbe(LifetimeProbe&& o) noexcept
      : large_payload(std::move(o.large_payload)) {
    g_probe_constructed_count.fetch_add(1);
  }
  ~LifetimeProbe() { g_probe_destructed_count.fetch_add(1); }
};

// 3. 辅助深度 DAG 算子定义
static std::mutex s_deep_dag_mutex;
static std::vector<std::string> s_deep_dag_trace;

static void ResetDeepDagTrace() {
  std::lock_guard<std::mutex> lock(s_deep_dag_mutex);
  s_deep_dag_trace.clear();
}

static void AppendDeepDagTrace(const std::string& node_name) {
  std::lock_guard<std::mutex> lock(s_deep_dag_mutex);
  s_deep_dag_trace.push_back(node_name);
}

static std::vector<std::string> SnapshotDeepDagTrace() {
  std::lock_guard<std::mutex> lock(s_deep_dag_mutex);
  return s_deep_dag_trace;
}

class DeepDagNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "DeepDagNode";
  bool Init(const NodeInitContext& init_ctx) override {
    if (!init_ctx.config || !init_ctx.session_ctx) return false;
    name_ = init_ctx.config->value("node_name", "DeepDagNode");
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    AppendDeepDagTrace(name_);
    req_ctx->Publish("out_" + name_, std::string("DataFrom_") + name_);
    return 0;
  }

  NodeControlResult Control(int cmd, const std::string& param) override {
    (void)cmd;
    (void)param;
    return NodeControlResult::Handled(0);
  }

  const std::string& Name() const override { return name_; }

 private:
  std::string name_;
};

inline NodeDefinition MakeDeepDagNodeDef() {
  NodeDefinition def;
  def.node_type = DeepDagNode::kNodeType;
  def.category = "test";
  def.description = "test deep dag node";
  def.config_fields = {ConfigFieldDefinition{
      "node_name", ConfigValueKind::kString, false, "DeepDagNode"}};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(DeepDagNode, MakeDeepDagNodeDef());

}  // namespace alg_framework

// -----------------------------------------------------------------------------
// GTest 测试套件
// -----------------------------------------------------------------------------
class EngineFaultToleranceAndLifecycleTest : public ::testing::Test {};

// 1. 硬件故障注入与定长批调度器错误阻断测试
TEST_F(EngineFaultToleranceAndLifecycleTest,
       HardwareFaultInjectionAndErrorPropagation) {
  using namespace alg_framework;

  auto model = std::make_shared<MockFaultyHardwareModel>();
  model->SetFault(true, -505);

  std::vector<TraceableItem<std::string>> input_items;
  for (int i = 0; i < 5; ++i) {
    TraceableItem<std::string> item;
    item.req_id = 100U + static_cast<uint32_t>(i);
    item.sub_id = 0U;
    item.data = "query_" + std::to_string(i);
    input_items.push_back(std::move(item));
  }

  std::vector<TraceableItem<std::vector<float>>> output_embeddings;

  // 执行批推理，底层硬件故障应被拦截并返回 -505
  int ret = model->Embed(input_items, EmbeddingOptions{}, &output_embeddings);
  EXPECT_EQ(ret, -505);

  // 恢复硬件正常状态后重试
  model->SetFault(false);
  output_embeddings.clear();
  ret = model->Embed(input_items, EmbeddingOptions{}, &output_embeddings);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(output_embeddings.size(), 5U);
  EXPECT_EQ(output_embeddings[0].data.size(), 128U);
  EXPECT_EQ(output_embeddings[4].data.size(), 128U);
}

// 2. 5 层深度复杂波前 DAG 拓扑执行测试 (Layer 0 ~ Layer 4)
TEST_F(EngineFaultToleranceAndLifecycleTest, Deep5LayerWavefrontDagExecution) {
  using namespace alg_framework;

  // 构建 5 层 11 节点复杂 DAG 图:
  // Layer 0: R1, R2
  // Layer 1: A1, A2, A3 (依赖 R1, R2)
  // Layer 2: M1, M2 (依赖 A1, A2, A3)
  // Layer 3: B1, B2, B3 (依赖 M1, M2)
  // Layer 4: Final (依赖 B1, B2, B3)
  nlohmann::json deep_dag_config = {{"biz_name", "deep_5_layer_dag"},
                                    {"execution_mode", "parallel"},
                                    {"max_parallel_workers", 4},
                                    {"pipeline",
                                     {{{"id", "R1"},
                                       {"node_type", "DeepDagNode"},
                                       {"config", {{"node_name", "R1"}}},
                                       {"depends_on", nlohmann::json::array()}},
                                      {{"id", "R2"},
                                       {"node_type", "DeepDagNode"},
                                       {"config", {{"node_name", "R2"}}},
                                       {"depends_on", nlohmann::json::array()}},

                                      {{"id", "A1"},
                                       {"node_type", "DeepDagNode"},
                                       {"config", {{"node_name", "A1"}}},
                                       {"depends_on", {"R1"}}},
                                      {{"id", "A2"},
                                       {"node_type", "DeepDagNode"},
                                       {"config", {{"node_name", "A2"}}},
                                       {"depends_on", {"R1", "R2"}}},
                                      {{"id", "A3"},
                                       {"node_type", "DeepDagNode"},
                                       {"config", {{"node_name", "A3"}}},
                                       {"depends_on", {"R2"}}},

                                      {{"id", "M1"},
                                       {"node_type", "DeepDagNode"},
                                       {"config", {{"node_name", "M1"}}},
                                       {"depends_on", {"A1", "A2"}}},
                                      {{"id", "M2"},
                                       {"node_type", "DeepDagNode"},
                                       {"config", {{"node_name", "M2"}}},
                                       {"depends_on", {"A2", "A3"}}},

                                      {{"id", "B1"},
                                       {"node_type", "DeepDagNode"},
                                       {"config", {{"node_name", "B1"}}},
                                       {"depends_on", {"M1"}}},
                                      {{"id", "B2"},
                                       {"node_type", "DeepDagNode"},
                                       {"config", {{"node_name", "B2"}}},
                                       {"depends_on", {"M1", "M2"}}},
                                      {{"id", "B3"},
                                       {"node_type", "DeepDagNode"},
                                       {"config", {{"node_name", "B3"}}},
                                       {"depends_on", {"M2"}}},

                                      {{"id", "Final"},
                                       {"node_type", "DeepDagNode"},
                                       {"config", {{"node_name", "Final"}}},
                                       {"depends_on", {"B1", "B2", "B3"}}}}}};

  Pipeline pipeline;
  ASSERT_TRUE(pipeline.BuildFromJson(
      deep_dag_config, nullptr, ValidationPolicy::kPrivateExtensionCompatible));
  EXPECT_EQ(pipeline.GetExecutionMode(), Pipeline::ExecutionMode::PARALLEL);

  const auto& layers = pipeline.GetTopologicalLayers();
  ASSERT_EQ(layers.size(), 5);
  EXPECT_EQ(layers[0].size(), 2);  // Layer 0: R1, R2
  EXPECT_EQ(layers[1].size(), 3);  // Layer 1: A1, A2, A3
  EXPECT_EQ(layers[2].size(), 2);  // Layer 2: M1, M2
  EXPECT_EQ(layers[3].size(), 3);  // Layer 3: B1, B2, B3
  EXPECT_EQ(layers[4].size(), 1);  // Layer 4: Final

  AlgContext req_ctx;
  ResetDeepDagTrace();

  int ret = pipeline.Execute(&req_ctx);
  EXPECT_EQ(ret, 0);

  const auto trace = SnapshotDeepDagTrace();
  EXPECT_EQ(trace.size(), 11);

  // 验证所有节点的黑板产出均已写入
  EXPECT_TRUE(req_ctx.Has("out_R1"));
  EXPECT_TRUE(req_ctx.Has("out_R2"));
  EXPECT_TRUE(req_ctx.Has("out_A2"));
  EXPECT_TRUE(req_ctx.Has("out_M1"));
  EXPECT_TRUE(req_ctx.Has("out_B2"));
  EXPECT_TRUE(req_ctx.Has("out_Final"));
}

// 3. 大负载请求黑板 RAII 确定性析构与内存回收压测
TEST_F(EngineFaultToleranceAndLifecycleTest, LargePayloadRaiiDestruction) {
  using namespace alg_framework;

  g_probe_constructed_count.store(0);
  g_probe_destructed_count.store(0);

  const int test_cycles = 50;
  for (int i = 0; i < test_cycles; ++i) {
    {
      AlgContext ctx;
      ctx.Publish("probe_1", LifetimeProbe(20000));
      ctx.Publish("probe_2", LifetimeProbe(20000));

      EXPECT_TRUE(ctx.Has("probe_1"));
      EXPECT_TRUE(ctx.Has("probe_2"));
      auto* p1 = ctx.Read<LifetimeProbe>("probe_1");
      ASSERT_NE(p1, nullptr);
      EXPECT_EQ(p1->large_payload.size(), 20000);
      // 作用域退出，ctx 销毁
    }
  }

  // 严格断言：构造次数与析构次数 100% 精确相等，且大于 0，无任何内存泄漏
  EXPECT_GT(g_probe_constructed_count.load(), 0);
  EXPECT_EQ(g_probe_constructed_count.load(), g_probe_destructed_count.load());
}

// 4. 全局生命周期高频循环初始化与销毁压测 (30 Cycles)
TEST_F(EngineFaultToleranceAndLifecycleTest, RapidGlobalLifecycleInitDeInit) {
  std::string cfg_path = GetConfigPath("configs/pipeline_keyword_match.json");

  for (int cycle = 0; cycle < 30; ++cycle) {
    EXPECT_EQ(Alg_Init(), 0);

    CompanyAlgParamCreate param;
    param.config_file_path = cfg_path.c_str();
    param.model_root_dir = "./models";
    param.device_id = 0;
    param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

    void* handle = nullptr;
    ASSERT_EQ(Alg_Create(&handle, &param), 0);
    ASSERT_NE(handle, nullptr);

    const char* input_text = "循环生命周期压测文本";
    CompanyKeywordInputStruct in_req{static_cast<uint64_t>(10000 + cycle),
                                     input_text};
    std::vector<void*> inputs = {&in_req};
    CompanyKeywordOutputStruct out_res;
    std::vector<void*> outputs = {&out_res};

    int ret = Alg_Process(handle, inputs, outputs);
    EXPECT_EQ(ret, 0);

    EXPECT_EQ(Alg_Destroy(handle), 0);
    EXPECT_EQ(Alg_DeInit(), 0);
  }
}

#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "company_alg_interface.h"
#include "core/alg_context.h"
#include "core/node_registry.h"
#include "core/session_context.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"

namespace alg_framework {

/**
 * @brief 专用于测试 RerankRefineNode 重排序与 Top-K 行为的可控 Mock 引擎
 */
class ControllableMockRerankEngine : public IRerankEngine {
 public:
  bool Load(const std::string& model_path,
            const nlohmann::json& config) override {
    (void)model_path;
    (void)config;
    return true;
  }

  int ScoreTraceableBatch(
      const std::vector<TraceableItem<PairInput>>& input_pairs,
      std::vector<TraceableItem<float>>* output_scores) override {
    if (!output_scores) return -1;
    output_scores->clear();
    for (const auto& item : input_pairs) {
      float score = 0.5f;
      if (item.data.passage.find("SCORE_0.9") != std::string::npos) {
        score = 0.9f;
      } else if (item.data.passage.find("SCORE_0.7") != std::string::npos) {
        score = 0.7f;
      } else if (item.data.passage.find("SCORE_0.3") != std::string::npos) {
        score = 0.3f;
      } else if (item.data.passage.find("SCORE_0.1") != std::string::npos) {
        score = 0.1f;
      }
      output_scores->emplace_back(item.req_id, item.sub_id, score);
    }
    return 0;
  }

  size_t GetMaxBatchSize() const override { return 16; }

  const std::string& EngineType() const override {
    static const std::string type = "controllable_mock_rerank";
    return type;
  }
};

class RerankRefineNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Alg_Init();
    mock_engine_ = std::make_shared<ControllableMockRerankEngine>();
    session_ctx_.GetModelManager().RegisterModel("test_rerank_model",
                                                 mock_engine_);
    node_ = NodeFactory::Instance().Create("RerankRefineNode");
    ASSERT_NE(node_, nullptr);
  }

  void TearDown() override { Alg_DeInit(); }

  std::shared_ptr<ControllableMockRerankEngine> mock_engine_;
  SessionContext session_ctx_;
  std::unique_ptr<INode> node_;
};

// 1. 验证重排序与 Top-K 选精逻辑 (打分最高的候选排在前面)
TEST_F(RerankRefineNodeTest, ReorderAndTopKFiltering) {
  nlohmann::json cfg = {
      {"bind_model", "test_rerank_model"},      {"top_k", 2},
      {"candidates_key", "matched_top_chunks"}, {"query_key", "raw_queries"},
      {"output_key", "matched_top_chunks"},
  };
  ASSERT_TRUE(node_->Init(cfg, &session_ctx_));

  AlgContext ctx;
  std::vector<std::string> raw_queries = {"什么是深度学习？", "退款流程？"};
  ctx.Set("raw_queries", raw_queries);

  // 故意将低分样本放在前面，高分样本放在后面，测试重排序
  std::vector<TraceableItem<std::string>> input_candidates = {
      // Request 0 的 3 个候选 (期望排序: sub_id=2 (0.9) -> sub_id=1 (0.7))
      {0, 0, "候选 A (SCORE_0.1)"},
      {0, 1, "候选 B (SCORE_0.7)"},
      {0, 2, "候选 C (SCORE_0.9)"},
      // Request 1 的 2 个候选 (期望排序: sub_id=1 (0.9) -> sub_id=0 (0.3))
      {1, 0, "候选 D (SCORE_0.3)"},
      {1, 1, "候选 E (SCORE_0.9)"},
  };
  ctx.Set("matched_top_chunks", input_candidates);

  int ret = node_->Process(&ctx);
  EXPECT_EQ(ret, 0);

  auto* result =
      ctx.Get<std::vector<TraceableItem<std::string>>>("matched_top_chunks");
  ASSERT_NE(result, nullptr);
  // 每个请求保留 top_k=2，共 4 个输出
  ASSERT_EQ(result->size(), 4);

  // Request 0: 验证 Top-1 是 SCORE_0.9, Top-2 是 SCORE_0.7
  EXPECT_EQ((*result)[0].req_id, 0);
  EXPECT_EQ((*result)[0].sub_id, 2);
  EXPECT_EQ((*result)[0].data, "候选 C (SCORE_0.9)");

  EXPECT_EQ((*result)[1].req_id, 0);
  EXPECT_EQ((*result)[1].sub_id, 1);
  EXPECT_EQ((*result)[1].data, "候选 B (SCORE_0.7)");

  // Request 1: 验证 Top-1 是 SCORE_0.9, Top-2 是 SCORE_0.3
  EXPECT_EQ((*result)[2].req_id, 1);
  EXPECT_EQ((*result)[2].sub_id, 1);
  EXPECT_EQ((*result)[2].data, "候选 E (SCORE_0.9)");

  EXPECT_EQ((*result)[3].req_id, 1);
  EXPECT_EQ((*result)[3].sub_id, 0);
  EXPECT_EQ((*result)[3].data, "候选 D (SCORE_0.3)");
}

// 2. 验证空候选集鲁棒性
TEST_F(RerankRefineNodeTest, EmptyCandidatesHandling) {
  nlohmann::json cfg = {{"bind_model", "test_rerank_model"}, {"top_k", 1}};
  ASSERT_TRUE(node_->Init(cfg, &session_ctx_));

  AlgContext ctx;
  std::vector<std::string> raw_queries = {"Query"};
  ctx.Set("raw_queries", raw_queries);
  ctx.Set("matched_top_chunks", std::vector<TraceableItem<std::string>>{});

  int ret = node_->Process(&ctx);
  EXPECT_EQ(ret, 0);
}

// 3. 验证缺失黑板 Key 拦截
TEST_F(RerankRefineNodeTest, MissingContextKeyHandling) {
  nlohmann::json cfg = {{"bind_model", "test_rerank_model"}};
  ASSERT_TRUE(node_->Init(cfg, &session_ctx_));

  AlgContext ctx;  // 空黑板
  int ret = node_->Process(&ctx);
  EXPECT_EQ(ret, -3201);
}

}  // namespace alg_framework

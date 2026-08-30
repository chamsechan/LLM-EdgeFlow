#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "company_alg_interface.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/session_context.h"
#include "core/traceable_item.h"
#include "engine/model_interface.h"
#include "tests/support/node_test_utils.h"

namespace alg_framework {

/**
 * @brief 专用于测试 TextRerankNode 重排序与 Top-K 行为的可控 Mock 引擎
 */
class ControllableMockRerankModel : public IRerankModel {
 public:
  const std::string& ModelType() const noexcept override {
    static const std::string type = "controllable_mock_rerank";
    return type;
  }
  const std::string& Capability() const noexcept override {
    static const std::string capability = "rerank";
    return capability;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  size_t GetMaxBatchSize() const noexcept override { return 16; }

  int Score(const QueryCandidatesBatch& input_pairs,
            ScoreBatch* output_scores) noexcept override {
    if (!output_scores) return -1;
    output_scores->clear();
    for (const auto& item : input_pairs) {
      float score = 0.5f;
      if (item.data.candidate.find("SCORE_0.9") != std::string::npos) {
        score = 0.9f;
      } else if (item.data.candidate.find("SCORE_0.7") != std::string::npos) {
        score = 0.7f;
      } else if (item.data.candidate.find("SCORE_0.3") != std::string::npos) {
        score = 0.3f;
      } else if (item.data.candidate.find("SCORE_0.1") != std::string::npos) {
        score = 0.1f;
      }
      output_scores->emplace_back(item.req_id, item.sub_id, score);
    }
    return 0;
  }
};

class TextRerankNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Alg_Init();
    mock_model_ = std::make_shared<ControllableMockRerankModel>();
    session_ctx_.GetModelManager().RegisterModel("test_rerank_model",
                                                 mock_model_, "test-v1");
    node_ = NodeFactory::Instance().Create("TextRerankNode");
    ASSERT_NE(node_, nullptr);
  }

  void TearDown() override { Alg_DeInit(); }

  std::shared_ptr<ControllableMockRerankModel> mock_model_;
  SessionContext session_ctx_;
  std::unique_ptr<INode> node_;
};

// 1. 验证重排序与 Top-K 选精逻辑 (打分最高的候选排在前面)
TEST_F(TextRerankNodeTest, ReorderAndTopKFiltering) {
  nlohmann::json cfg = {
      {"bind_model", "test_rerank_model"},
      {"top_k", 2},
  };
  ASSERT_TRUE(InitNodeForTest(*node_, cfg, &session_ctx_));

  AlgContext ctx;
  TextBatch raw_queries = {
      TraceableItem<std::string>{0, 0, "什么是深度学习？"},
      TraceableItem<std::string>{1, 0, "退款流程？"},
  };
  ctx.Publish(BlackboardKey<TextBatch>{"queries", "TextBatch"}, raw_queries);

  // 故意将低分样本放在前面，高分样本放在后面，测试重排序
  RankedTextBatch input_candidates = {
      // Request 0 的 3 个候选 (期望排序: sub_id=2 (0.9) -> sub_id=1 (0.7))
      {0, 0, RankedCandidate("候选 A (SCORE_0.1)", 0.1f, 1, 0)},
      {0, 1, RankedCandidate("候选 B (SCORE_0.7)", 0.7f, 2, 1)},
      {0, 2, RankedCandidate("候选 C (SCORE_0.9)", 0.9f, 3, 2)},
      // Request 1 的 2 个候选 (期望排序: sub_id=1 (0.9) -> sub_id=0 (0.3))
      {1, 0, RankedCandidate("候选 D (SCORE_0.3)", 0.3f, 1, 0)},
      {1, 1, RankedCandidate("候选 E (SCORE_0.9)", 0.9f, 2, 1)},
  };
  ctx.Publish(BlackboardKey<RankedTextBatch>{"candidates", "RankedTextBatch"},
              input_candidates);

  int ret = node_->Process(&ctx);
  EXPECT_EQ(ret, 0);

  auto* result =
      ctx.Read(BlackboardKey<RankedTextBatch>{"ranked", "RankedTextBatch"});
  ASSERT_NE(result, nullptr);
  // 每个请求保留 top_k=2，共 4 个输出
  ASSERT_EQ(result->size(), 4U);

  // Request 0: 验证 Top-1 是 SCORE_0.9, Top-2 是 SCORE_0.7
  EXPECT_EQ((*result)[0].req_id, 0U);
  EXPECT_EQ((*result)[0].data.original_sub_id, 2U);
  EXPECT_EQ((*result)[0].data.text, "候选 C (SCORE_0.9)");

  EXPECT_EQ((*result)[1].req_id, 0U);
  EXPECT_EQ((*result)[1].data.original_sub_id, 1U);
  EXPECT_EQ((*result)[1].data.text, "候选 B (SCORE_0.7)");

  // Request 1: 验证 Top-1 是 SCORE_0.9, Top-2 是 SCORE_0.3
  EXPECT_EQ((*result)[2].req_id, 1U);
  EXPECT_EQ((*result)[2].data.original_sub_id, 1U);
  EXPECT_EQ((*result)[2].data.text, "候选 E (SCORE_0.9)");

  EXPECT_EQ((*result)[3].req_id, 1U);
  EXPECT_EQ((*result)[3].data.original_sub_id, 0U);
  EXPECT_EQ((*result)[3].data.text, "候选 D (SCORE_0.3)");
}

// 2. 验证空候选集鲁棒性
TEST_F(TextRerankNodeTest, EmptyCandidatesHandling) {
  nlohmann::json cfg = {{"bind_model", "test_rerank_model"}, {"top_k", 1}};
  ASSERT_TRUE(InitNodeForTest(*node_, cfg, &session_ctx_));

  AlgContext ctx;
  TextBatch raw_queries = {TraceableItem<std::string>{0, 0, "Query"}};
  ctx.Publish(BlackboardKey<TextBatch>{"queries", "TextBatch"}, raw_queries);
  ctx.Publish(BlackboardKey<RankedTextBatch>{"candidates", "RankedTextBatch"},
              RankedTextBatch{});

  int ret = node_->Process(&ctx);
  EXPECT_EQ(ret, 0);
}

// 3. 验证缺失黑板 Key 拦截
TEST_F(TextRerankNodeTest, MissingContextKeyHandling) {
  nlohmann::json cfg = {{"bind_model", "test_rerank_model"}};
  ASSERT_TRUE(InitNodeForTest(*node_, cfg, &session_ctx_));

  AlgContext ctx;  // 空黑板
  int ret = node_->Process(&ctx);
  EXPECT_EQ(ret, -7001);
}

}  // namespace alg_framework

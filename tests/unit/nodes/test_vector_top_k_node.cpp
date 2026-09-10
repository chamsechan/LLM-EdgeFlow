#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapter/shared_algorithm_runtime.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/session_context.h"
#include "tests/support/node_test_utils.h"

namespace llm_edgeflow {

class VectorTopKNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SharedAlgorithmRuntime::GlobalInit(), 0);
    session_ctx_ = std::make_unique<SessionContext>();
  }
  std::unique_ptr<SessionContext> session_ctx_;
};

// 1. Init & Config Validation
TEST_F(VectorTopKNodeTest, InitAndConfigValidation) {
  auto node = NodeRegistry::Instance().Create("VectorTopKNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"top_k", 2}, {"min_score", 0.0}, {"metric", "cosine"}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  auto invalid_node1 = NodeRegistry::Instance().Create("VectorTopKNode");
  ASSERT_NE(invalid_node1, nullptr);
  EXPECT_FALSE(
      InitNodeForTest(*invalid_node1, {{"top_k", -1}}, session_ctx_.get()));

  auto invalid_node2 = NodeRegistry::Instance().Create("VectorTopKNode");
  ASSERT_NE(invalid_node2, nullptr);
  EXPECT_FALSE(InitNodeForTest(*invalid_node2, {{"metric", "invalid_metric"}},
                               session_ctx_.get()));

  auto invalid_node3 = NodeRegistry::Instance().Create("VectorTopKNode");
  ASSERT_NE(invalid_node3, nullptr);
  EXPECT_FALSE(
      InitNodeForTest(*invalid_node3, {{"top_k", 2.5}}, session_ctx_.get()));
}

// 2. Process Top-K Ranking with Shared Candidates
TEST_F(VectorTopKNodeTest, ProcessRankingSharedCandidates) {
  auto node = NodeRegistry::Instance().Create("VectorTopKNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(
      *node, {{"top_k", 2}, {"min_score", 0.0}, {"candidate_scope", "shared"}},
      session_ctx_.get()));

  AlgContext ctx;
  EmbeddingBatch queries;
  queries.emplace_back(1, 0, std::vector<float>{1.0f, 0.0f, 0.0f});

  EmbeddingBatch candidates;
  candidates.emplace_back(0, 0, std::vector<float>{0.9f, 0.1f, 0.0f});
  candidates.emplace_back(0, 1, std::vector<float>{0.0f, 1.0f, 0.0f});
  candidates.emplace_back(0, 2, std::vector<float>{0.5f, 0.5f, 0.0f});

  TextBatch cand_texts;
  cand_texts.emplace_back(0, 0, "Doc A (High Sim)");
  cand_texts.emplace_back(0, 1, "Doc B (Low Sim)");
  cand_texts.emplace_back(0, 2, "Doc C (Mid Sim)");

  ctx.Publish("queries", queries);
  ctx.Publish("candidates", candidates);
  ctx.Publish("candidate_texts", cand_texts);

  EXPECT_EQ(node->Process(&ctx), 0);

  const auto* ranked = ctx.Read<RankedTextBatch>("ranked");
  ASSERT_NE(ranked, nullptr);
  ASSERT_EQ(ranked->size(), 2u);
  EXPECT_EQ((*ranked)[0].data.text, "Doc A (High Sim)");
  EXPECT_EQ((*ranked)[1].data.text, "Doc C (Mid Sim)");
}

// 3. Missing Queries Fails Closed
TEST_F(VectorTopKNodeTest, MissingInputFailsClosed) {
  auto node = NodeRegistry::Instance().Create("VectorTopKNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"top_k", 2}}, session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_EQ(node->Process(&empty_ctx), -3101);
}

}  // namespace llm_edgeflow

namespace llm_edgeflow {
TEST_F(VectorTopKNodeTest, PrivateRequestZeroCandidatesAreNotBroadcast) {
  auto node = NodeRegistry::Instance().Create("VectorTopKNode");
  ASSERT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));
  AlgContext ctx;
  ctx.Publish("queries", EmbeddingBatch{{1, 0, {1.0f}}});
  ctx.Publish("candidates", EmbeddingBatch{{0, 0, {1.0f}}});
  ASSERT_EQ(node->Process(&ctx), 0);
  ASSERT_NE(ctx.Read<RankedTextBatch>("ranked"), nullptr);
  EXPECT_TRUE(ctx.Read<RankedTextBatch>("ranked")->empty());
}
TEST_F(VectorTopKNodeTest, RejectsDimensionMismatch) {
  auto node = NodeRegistry::Instance().Create("VectorTopKNode");
  ASSERT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));
  AlgContext ctx;
  ctx.Publish("queries", EmbeddingBatch{{0, 0, {1.0f}}});
  ctx.Publish("candidates", EmbeddingBatch{{0, 0, {1.0f, 0.0f}}});
  EXPECT_NE(node->Process(&ctx), 0);
  EXPECT_FALSE(ctx.Has("ranked"));
}
}  // namespace llm_edgeflow

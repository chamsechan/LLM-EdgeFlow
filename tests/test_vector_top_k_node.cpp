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

namespace alg_framework {

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
  auto node = NodeFactory::Instance().Create("VectorTopKNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"top_k", 2}, {"min_score", 0.0}, {"metric", "cosine"}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));
}

// 2. Process Top-K Ranking with Shared Candidates
TEST_F(VectorTopKNodeTest, ProcessRankingSharedCandidates) {
  auto node = NodeFactory::Instance().Create("VectorTopKNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      node->Init({{"top_k", 2}, {"min_score", 0.0}}, session_ctx_.get()));

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

  ctx.Set("queries", queries);
  ctx.Set("candidates", candidates);
  ctx.Set("candidate_texts", cand_texts);

  EXPECT_EQ(node->Process(&ctx), 0);

  const auto* ranked = ctx.Get<RankedTextBatch>("ranked");
  ASSERT_NE(ranked, nullptr);
  ASSERT_EQ(ranked->size(), 2u);
  EXPECT_EQ((*ranked)[0].data.text, "Doc A (High Sim)");
  EXPECT_EQ((*ranked)[1].data.text, "Doc C (Mid Sim)");
}

// 3. Missing Queries Fails Closed
TEST_F(VectorTopKNodeTest, MissingInputFailsClosed) {
  auto node = NodeFactory::Instance().Create("VectorTopKNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(node->Init({{"top_k", 2}}, session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_NE(node->Process(&empty_ctx), 0);
}

}  // namespace alg_framework

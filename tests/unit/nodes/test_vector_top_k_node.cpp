#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline_validator.h"
#include "core/session_context.h"
#include "nodes/node_error_codes.h"
#include "tests/support/node_test_utils.h"
#include "tests/support/pipeline_test_utils.h"

namespace llm_edgeflow {

class VectorTopKNodeTest : public ::testing::Test {
 protected:
  void SetUp() override { session_ctx_ = std::make_unique<SessionContext>(); }
  std::unique_ptr<SessionContext> session_ctx_;
};

TEST_F(VectorTopKNodeTest, InitAndConfigValidation) {
  auto node = NodeRegistry::Instance().Create("vector_top_k");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"top_k", 2}, {"min_score", 0.0}, {"metric", "cosine"}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  auto invalid_node1 = NodeRegistry::Instance().Create("vector_top_k");
  ASSERT_NE(invalid_node1, nullptr);
  EXPECT_FALSE(
      InitNodeForTest(*invalid_node1, {{"top_k", -1}}, session_ctx_.get()));

  auto invalid_node2 = NodeRegistry::Instance().Create("vector_top_k");
  ASSERT_NE(invalid_node2, nullptr);
  EXPECT_FALSE(InitNodeForTest(*invalid_node2, {{"metric", "invalid_metric"}},
                               session_ctx_.get()));

  auto invalid_node3 = NodeRegistry::Instance().Create("vector_top_k");
  ASSERT_NE(invalid_node3, nullptr);
  EXPECT_FALSE(
      InitNodeForTest(*invalid_node3, {{"top_k", 2.5}}, session_ctx_.get()));
}

TEST_F(VectorTopKNodeTest, ProcessRankingSharedCandidates) {
  auto node = NodeRegistry::Instance().Create("vector_top_k");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"top_k", 2}, {"min_score", 0.0}},
                              session_ctx_.get(), nullptr, {},
                              {{"candidates", "session"}}));

  AlgContext ctx;
  EmbeddingBatch queries;
  queries.emplace_back(1, 0, std::vector<float>{1.0f, 0.0f, 0.0f});

  EmbeddingBatch candidates;
  candidates.emplace_back(0, 0, std::vector<float>{1.0f, 0.0f, 0.0f});
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
  EXPECT_FLOAT_EQ((*ranked)[0].data.score, 1.0f);
  EXPECT_EQ((*ranked)[1].data.text, "Doc C (Mid Sim)");
}

TEST_F(VectorTopKNodeTest, MissingInputFailsClosed) {
  auto node = NodeRegistry::Instance().Create("vector_top_k");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"top_k", 2}}, session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_EQ(node->Process(&empty_ctx), node_error::author_node::kMissingInput);
}

}  // namespace llm_edgeflow

namespace llm_edgeflow {
TEST_F(VectorTopKNodeTest, PrivateRequestZeroCandidatesAreNotBroadcast) {
  auto node = NodeRegistry::Instance().Create("vector_top_k");
  ASSERT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));
  AlgContext ctx;
  ctx.Publish("queries", EmbeddingBatch{{1, 0, {1.0f}}});
  ctx.Publish("candidates", EmbeddingBatch{{0, 0, {1.0f}}});
  ctx.Publish("candidate_texts", TextBatch{{0, 0, "candidate"}});
  ASSERT_EQ(node->Process(&ctx), 0);
  ASSERT_NE(ctx.Read<RankedTextBatch>("ranked"), nullptr);
  EXPECT_TRUE(ctx.Read<RankedTextBatch>("ranked")->empty());
}
TEST_F(VectorTopKNodeTest, RejectsDimensionMismatch) {
  auto node = NodeRegistry::Instance().Create("vector_top_k");
  ASSERT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));
  AlgContext ctx;
  ctx.Publish("queries", EmbeddingBatch{{0, 0, {1.0f}}});
  ctx.Publish("candidates", EmbeddingBatch{{0, 0, {1.0f, 0.0f}}});
  ctx.Publish("candidate_texts", TextBatch{{0, 0, "candidate"}});
  EXPECT_NE(node->Process(&ctx), 0);
  EXPECT_FALSE(ctx.Has("ranked"));
}
}  // namespace llm_edgeflow

namespace llm_edgeflow {
TEST_F(VectorTopKNodeTest, RejectsMultipleQueriesForSameRequest) {
  auto node = NodeRegistry::Instance().Create("vector_top_k");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, nlohmann::json::object(),
                              session_ctx_.get(), nullptr, {},
                              {{"candidates", "session"}}));
  AlgContext ctx;
  ctx.Publish("queries", EmbeddingBatch{{7, 0, {1.0f}}, {7, 1, {1.0f}}});
  ctx.Publish("candidates", EmbeddingBatch{{0, 0, {1.0f}}});
  ctx.Publish("candidate_texts", TextBatch{{0, 0, "candidate"}});

  EXPECT_EQ(node->Process(&ctx), -3102);
  EXPECT_FALSE(ctx.Has("ranked"));
}
}  // namespace llm_edgeflow

namespace llm_edgeflow {
TEST_F(VectorTopKNodeTest, CandidateTextsMustBeConnectedInThePlan) {
  const auto boundary = MakeTestBoundary(
      {{"input.queries", "EmbeddingBatch"},
       {"input.candidates", "EmbeddingBatch", true, "N:1"}},
      {{"retrieve.ranked", "RankedTextBatch", true, "1:N", "generate_sub_id"}});
  const nlohmann::json pipeline = {
      {"pipeline",
       {{{"type", "vector_top_k"},
         {"name", "retrieve"},
         {"inputs",
          {{"queries", "input.queries"},
           {"candidates", "input.candidates"}}}}}}};
  const auto plan = PipelineValidator::ValidateAndPlan(pipeline, boundary);
  EXPECT_FALSE(plan.report.ok);
  EXPECT_TRUE(std::any_of(
      plan.report.diagnostics.begin(), plan.report.diagnostics.end(),
      [](const auto& diagnostic) {
        return diagnostic.code == DiagnosticCode::kMissingInputProducer &&
               diagnostic.path == "/pipeline/0/inputs" &&
               diagnostic.message.find("candidate_texts") != std::string::npos;
      }))
      << plan.report.ToJson().dump(2);
}

TEST_F(VectorTopKNodeTest, RequestCandidatesStayWithinTheirRequest) {
  auto node = NodeRegistry::Instance().Create("vector_top_k");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));
  AlgContext context;
  context.Publish("queries",
                  EmbeddingBatch{{7, 3, {1.0f, 0.0f}}, {19, 8, {1.0f, 0.0f}}});
  context.Publish("candidates",
                  EmbeddingBatch{{7, 5, {0.8f, 0.2f}}, {19, 6, {1.0f, 0.0f}}});
  context.Publish("candidate_texts", TextBatch{{7, 5, "request seven"},
                                               {19, 6, "request nineteen"}});
  ASSERT_EQ(node->Process(&context), 0) << context.GetErrorMessage();
  const auto* output = context.Read<RankedTextBatch>("ranked");
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), 2U);
  EXPECT_EQ((*output)[0].req_id, 7U);
  EXPECT_EQ((*output)[0].data.text, "request seven");
  EXPECT_EQ((*output)[0].data.original_sub_id, 5U);
  EXPECT_EQ((*output)[1].req_id, 19U);
  EXPECT_EQ((*output)[1].data.text, "request nineteen");
  EXPECT_EQ((*output)[1].data.original_sub_id, 6U);
}

TEST_F(VectorTopKNodeTest, SessionCandidatesAreSharedAndRequireRequestZero) {
  auto node = NodeRegistry::Instance().Create("vector_top_k");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, nlohmann::json::object(),
                              session_ctx_.get(), nullptr, {},
                              {{"candidates", "session"}}));
  AlgContext context;
  context.Publish("queries", EmbeddingBatch{{7, 3, {1.0f}}, {19, 8, {1.0f}}});
  context.Publish("candidates", EmbeddingBatch{{0, 5, {1.0f}}});
  context.Publish("candidate_texts", TextBatch{{0, 5, "shared"}});
  ASSERT_EQ(node->Process(&context), 0) << context.GetErrorMessage();
  const auto* output = context.Read<RankedTextBatch>("ranked");
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), 2U);
  EXPECT_EQ((*output)[0].req_id, 7U);
  EXPECT_EQ((*output)[1].req_id, 19U);
  for (const auto& item : *output) {
    EXPECT_EQ(item.data.text, "shared");
    EXPECT_EQ(item.data.original_sub_id, 5U);
  }
  AlgContext invalid;
  invalid.Publish("queries", EmbeddingBatch{{7, 3, {1.0f}}});
  invalid.Publish("candidates", EmbeddingBatch{{7, 5, {1.0f}}});
  invalid.Publish("candidate_texts",
                  TextBatch{{7, 5, "invalid shared request"}});
  EXPECT_EQ(node->Process(&invalid), -3102);
  EXPECT_FALSE(invalid.Has("ranked"));
}
}  // namespace llm_edgeflow

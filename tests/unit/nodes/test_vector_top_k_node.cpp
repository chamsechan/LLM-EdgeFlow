#include <gtest/gtest.h>

#include <limits>

#include "contracts/error_codes.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "tests/support/node_probe.h"

namespace llm_edgeflow {
namespace {
using test_support::NodeProbe;

TEST(VectorTopKNodeTest, CosineFiltersThenRanksAndKeepsCandidateIdentity) {
  SessionContext session;
  NodeProbe node;
  ASSERT_TRUE(node.Init("vector_top_k", {{"top_k", 2}, {"min_score", 0.5}},
                        session,
                        {{"queries", "query", ""},
                         {"candidates", "vectors", ""},
                         {"candidate_texts", "texts", ""}},
                        {{"ranked", "result", ""}}));
  AlgContext context;
  ASSERT_TRUE(context.Publish("query",
                              EmbeddingBatch{{8, 21, {1, 0}}, {9, 0, {1, 0}}}));
  ASSERT_TRUE(context.Publish(
      "vectors",
      EmbeddingBatch{
          {8, 4, {3, 4}}, {8, 2, {0, 1}}, {8, 8, {2, 0}}, {8, 7, {-1, 0}}}));
  ASSERT_TRUE(context.Publish("texts", TextBatch{{8, 2, "orthogonal"},
                                                 {8, 8, "aligned"},
                                                 {8, 4, "angled"},
                                                 {8, 7, "opposite"}}));
  ASSERT_EQ(node.node->Process(&context), 0);
  const auto* ranked = context.Read<RankedTextBatch>("result");
  ASSERT_NE(ranked, nullptr);
  ASSERT_EQ(ranked->size(), 2u);
  EXPECT_EQ((*ranked)[0].req_id, 8u);
  EXPECT_EQ((*ranked)[0].sub_id, 0u);
  EXPECT_EQ((*ranked)[0].data.text, "aligned");
  EXPECT_FLOAT_EQ((*ranked)[0].data.score, 1.0f);
  EXPECT_EQ((*ranked)[0].data.rank, 1);
  EXPECT_EQ((*ranked)[0].data.original_sub_id, 8u);
  EXPECT_EQ((*ranked)[1].sub_id, 1u);
  EXPECT_EQ((*ranked)[1].data.text, "angled");
  EXPECT_FLOAT_EQ((*ranked)[1].data.score, 0.6f);
  EXPECT_EQ((*ranked)[1].data.rank, 2);
  EXPECT_EQ((*ranked)[1].data.original_sub_id, 4u);
}

TEST(VectorTopKNodeTest, DotProductUsesMagnitudeAndInclusiveScoreThreshold) {
  SessionContext session;
  NodeProbe node;
  ASSERT_TRUE(node.Init(
      "vector_top_k",
      {{"metric", "dot_product"}, {"top_k", 2}, {"min_score", 4}}, session,
      {{"queries", "query", ""},
       {"candidates", "vectors", ""},
       {"candidate_texts", "texts", ""}},
      {{"ranked", "result", ""}}));
  AlgContext context;
  ASSERT_TRUE(context.Publish("query", EmbeddingBatch{{2, 9, {2, 1}}}));
  ASSERT_TRUE(context.Publish(
      "vectors",
      EmbeddingBatch{{2, 6, {2, 0}}, {2, 7, {0, 5}}, {2, 8, {1, 0}}}));
  ASSERT_TRUE(context.Publish(
      "texts", TextBatch{{2, 6, "four"}, {2, 7, "five"}, {2, 8, "two"}}));
  ASSERT_EQ(node.node->Process(&context), 0);
  const auto* ranked = context.Read<RankedTextBatch>("result");
  ASSERT_NE(ranked, nullptr);
  ASSERT_EQ(ranked->size(), 2u);
  EXPECT_EQ((*ranked)[0].data.text, "five");
  EXPECT_FLOAT_EQ((*ranked)[0].data.score, 5);
  EXPECT_EQ((*ranked)[1].data.text, "four");
  EXPECT_FLOAT_EQ((*ranked)[1].data.score, 4);
}

TEST(VectorTopKNodeTest,
     SessionCandidatesUseRequestZeroAndAreReusedForEachQuery) {
  SessionContext session;
  NodeProbe node;
  ASSERT_TRUE(node.Init("vector_top_k", nlohmann::json::object(), session,
                        {{"queries", "query", ""},
                         {"candidates", "vectors", "session"},
                         {"candidate_texts", "texts", "session"}},
                        {{"ranked", "result", ""}}));
  AlgContext context;
  ASSERT_TRUE(context.Publish("query",
                              EmbeddingBatch{{12, 4, {0, 1}}, {3, 7, {1, 0}}}));
  ASSERT_TRUE(context.Publish("vectors",
                              EmbeddingBatch{{0, 9, {1, 0}}, {0, 5, {0, 1}}}));
  ASSERT_TRUE(context.Publish(
      "texts", TextBatch{{0, 5, "vertical"}, {0, 9, "horizontal"}}));
  ASSERT_EQ(node.node->Process(&context), 0);
  const auto* ranked = context.Read<RankedTextBatch>("result");
  ASSERT_NE(ranked, nullptr);
  ASSERT_EQ(ranked->size(), 2u);
  EXPECT_EQ((*ranked)[0].req_id, 12u);
  EXPECT_EQ((*ranked)[0].sub_id, 0u);
  EXPECT_EQ((*ranked)[0].data.text, "vertical");
  EXPECT_EQ((*ranked)[0].data.original_sub_id, 5u);
  EXPECT_EQ((*ranked)[1].req_id, 3u);
  EXPECT_EQ((*ranked)[1].data.text, "horizontal");
  AlgContext invalid;
  ASSERT_TRUE(invalid.Publish("query", EmbeddingBatch{{12, 4, {1, 0}}}));
  ASSERT_TRUE(invalid.Publish("vectors", EmbeddingBatch{{12, 0, {1, 0}}}));
  ASSERT_TRUE(invalid.Publish("texts", TextBatch{{12, 0, "not shared"}}));
  EXPECT_EQ(node.node->Process(&invalid), error::kInvalidInput);
  EXPECT_FALSE(invalid.Has("result"));
}

TEST(VectorTopKNodeTest,
     RejectsDuplicateQueriesInvalidVectorsAndMissingCandidateText) {
  struct Case {
    const char* name;
    EmbeddingBatch queries;
    EmbeddingBatch candidates;
    TextBatch texts;
  };
  const std::vector<Case> cases = {
      {"duplicate query",
       {{1, 0, {1, 0}}, {1, 4, {0, 1}}},
       {{1, 0, {1, 0}}},
       {{1, 0, "text"}}},
      {"dimension mismatch", {{1, 0, {1, 0}}}, {{1, 0, {1}}}, {{1, 0, "text"}}},
      {"nonfinite query",
       {{1, 0, {std::numeric_limits<float>::quiet_NaN(), 0}}},
       {{1, 0, {1, 0}}},
       {{1, 0, "text"}}},
      {"nonfinite candidate",
       {{1, 0, {1, 0}}},
       {{1, 0, {std::numeric_limits<float>::infinity(), 0}}},
       {{1, 0, "text"}}},
      {"missing text identity",
       {{1, 0, {1, 0}}},
       {{1, 8, {1, 0}}},
       {{1, 9, "wrong sub"}}},
  };
  SessionContext session;
  NodeProbe node;
  ASSERT_TRUE(node.Init("vector_top_k", nlohmann::json::object(), session,
                        {{"queries", "query", ""},
                         {"candidates", "vectors", ""},
                         {"candidate_texts", "texts", ""}},
                        {{"ranked", "result", ""}}));
  for (const auto& example : cases) {
    SCOPED_TRACE(example.name);
    AlgContext context;
    ASSERT_TRUE(context.Publish("query", example.queries));
    ASSERT_TRUE(context.Publish("vectors", example.candidates));
    ASSERT_TRUE(context.Publish("texts", example.texts));
    EXPECT_EQ(node.node->Process(&context), error::kInvalidInput);
    EXPECT_FALSE(context.Has("result"));
  }
}
}  // namespace
}  // namespace llm_edgeflow

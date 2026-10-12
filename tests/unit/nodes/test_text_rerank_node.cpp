#include <gtest/gtest.h>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "engine/model_interface.h"
#include "tests/support/node_probe.h"

namespace llm_edgeflow {
namespace {
class RecordingRerank final : public IRerankModel {
 public:
  const std::string& ImplName() const noexcept override { return impl; }
  const std::string& ModelType() const noexcept override { return type; }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  int Score(const QueryCandidatesBatch& inputs, ScoreBatch* output,
            std::string* diagnostic) noexcept override {
    if (diagnostic) diagnostic->clear();
    calls.push_back(inputs);
    output->clear();
    for (size_t i = 0; i < inputs.size(); ++i)
      output->emplace_back(inputs[i].req_id, inputs[i].sub_id, scores.at(i));
    return 0;
  }
  std::string impl = "node_recording_rerank", type = "rerank";
  std::vector<float> scores = {0.1f, 0.7f, 0.9f, 0.2f};
  std::vector<QueryCandidatesBatch> calls;
};

TEST(TextRerankNodeTest,
     BuildsPairsAndRanksWithinEachRequestUsingLiteralModelScores) {
  SessionContext session;
  auto model = std::make_shared<RecordingRerank>();
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "ranker", model));
  test_support::NodeProbe node;
  ASSERT_TRUE(node.Init(
      "text_rerank", {{"bind_model", "ranker"}, {"top_k", 2}}, session,
      {{"queries", "queries", ""}, {"candidates", "candidates", ""}},
      {{"ranked", "result", ""}}));
  AlgContext context;
  ASSERT_TRUE(context.Publish(
      "queries", TextBatch{{5, 8, "query five"}, {2, 3, "query two"}}));
  ASSERT_TRUE(context.Publish(
      "candidates",
      RankedTextBatch{{5, 0, RankedCandidate("five low", 8, 1, 777)},
                      {2, 9, RankedCandidate("two high", 3, 1, 999)},
                      {5, 4, RankedCandidate("five high", 1, 2, 444)},
                      {2, 2, RankedCandidate("two low", 7, 2, 222)}}));
  ASSERT_EQ(node.node->Process(&context), 0);
  ASSERT_EQ(model->calls.size(), 1u);
  const auto& pairs = model->calls[0];
  ASSERT_EQ(pairs.size(), 4u);
  EXPECT_EQ(pairs[0].data.query, "query five");
  EXPECT_EQ(pairs[0].data.candidate, "five low");
  EXPECT_EQ(pairs[1].data.query, "query two");
  EXPECT_EQ(pairs[1].sub_id, 9u);
  EXPECT_EQ(pairs[2].data.candidate, "five high");
  const auto* output = context.Read<RankedTextBatch>("result");
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), 4u);
  const std::vector<std::string> texts = {"two high", "two low", "five high",
                                          "five low"};
  const std::vector<float> scores = {0.7f, 0.2f, 0.9f, 0.1f};
  const std::vector<uint32_t> originals = {9, 2, 4, 0};
  for (size_t i = 0; i < output->size(); ++i) {
    EXPECT_EQ((*output)[i].req_id, i < 2 ? 2u : 5u);
    EXPECT_EQ((*output)[i].sub_id, i % 2);
    EXPECT_EQ((*output)[i].data.rank, static_cast<int>(i % 2 + 1));
    EXPECT_EQ((*output)[i].data.text, texts[i]);
    EXPECT_FLOAT_EQ((*output)[i].data.score, scores[i]);
    EXPECT_EQ((*output)[i].data.original_sub_id, originals[i]);
  }
}

TEST(TextRerankNodeTest, TopKIsAppliedPerRequestAfterReranking) {
  SessionContext session;
  auto model = std::make_shared<RecordingRerank>();
  model->scores = {0.2f, 0.9f, 0.8f};
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "ranker", model));
  test_support::NodeProbe node;
  ASSERT_TRUE(
      node.Init("text_rerank", {{"bind_model", "ranker"}}, session,
                {{"queries", "queries", ""}, {"candidates", "candidates", ""}},
                {{"ranked", "result", ""}}));
  AlgContext context;
  ASSERT_TRUE(
      context.Publish("queries", TextBatch{{1, 0, "one"}, {2, 0, "two"}}));
  ASSERT_TRUE(context.Publish(
      "candidates", RankedTextBatch{{1, 8, RankedCandidate("low")},
                                    {1, 3, RankedCandidate("high")},
                                    {2, 6, RankedCandidate("other")}}));
  ASSERT_EQ(node.node->Process(&context), 0);
  const auto* output = context.Read<RankedTextBatch>("result");
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), 2u);
  EXPECT_EQ((*output)[0].data.text, "high");
  EXPECT_EQ((*output)[0].data.original_sub_id, 3u);
  EXPECT_EQ((*output)[1].data.text, "other");
  EXPECT_EQ((*output)[1].req_id, 2u);
}

TEST(TextRerankNodeTest,
     DuplicateQueriesAndCandidatesWithoutAQueryFailBeforeScoring) {
  SessionContext session;
  auto model = std::make_shared<RecordingRerank>();
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "ranker", model));
  test_support::NodeProbe node;
  ASSERT_TRUE(
      node.Init("text_rerank", {{"bind_model", "ranker"}}, session,
                {{"queries", "queries", ""}, {"candidates", "candidates", ""}},
                {{"ranked", "result", ""}}));
  for (bool duplicate : {true, false}) {
    SCOPED_TRACE(duplicate);
    AlgContext context;
    const TextBatch queries = duplicate
                                  ? TextBatch{{1, 0, "first"}, {1, 4, "second"}}
                                  : TextBatch{{2, 0, "unrelated"}};
    ASSERT_TRUE(context.Publish("queries", queries));
    ASSERT_TRUE(context.Publish(
        "candidates", RankedTextBatch{{1, 8, RankedCandidate("candidate")}}));
    EXPECT_NE(node.node->Process(&context), 0);
    EXPECT_FALSE(context.Has("result"));
    EXPECT_TRUE(model->calls.empty());
  }
}
}  // namespace
}  // namespace llm_edgeflow

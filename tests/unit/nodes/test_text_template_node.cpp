#include <gtest/gtest.h>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "tests/support/node_probe.h"

namespace llm_edgeflow {
namespace {
using test_support::NodeProbe;

TEST(TextTemplateNodeTest,
     AggregatesAuxiliaryInputsByRequestAndPreservesPrimaryOrder) {
  SessionContext session;
  NodeProbe node;
  ASSERT_TRUE(
      node.Init("text_template",
                {{"template",
                  "{{primary}}|{{context}}|{{matches}}|{{document}}|{literal}"},
                 {"separator", ";"}},
                session,
                {{"primary", "primary", ""},
                 {"context", "context", ""},
                 {"matches", "matches", ""},
                 {"document", "document", ""}},
                {{"text", "result", ""}}))
      << node.diagnostic;
  AlgContext context;
  ASSERT_TRUE(context.Publish(
      "primary", TextBatch{{7, 8, "P8"}, {2, 9, "P2"}, {7, 3, "P3"}}));
  ASSERT_TRUE(context.Publish("context",
                              RankedTextBatch{{7, 9, RankedCandidate("C1")},
                                              {2, 4, RankedCandidate("C2")},
                                              {7, 1, RankedCandidate("C3")}}));
  ASSERT_TRUE(context.Publish(
      "matches", RuleMatchBatch{{7, 4, RuleMatchItem(1, "route", "VIP")},
                                {2, 5, RuleMatchItem(1, "other", "")},
                                {7, 5, RuleMatchItem(1, "", "word")}}));
  ASSERT_TRUE(context.Publish(
      "document", OcrDocumentBatch{{7, 6, OcrDocumentItem{{}, "D1"}},
                                   {7, 2, OcrDocumentItem{{}, "D2"}},
                                   {2, 0, OcrDocumentItem{{}, "D3"}}}));
  ASSERT_EQ(node.node->Process(&context), 0);
  const auto* output = context.Read<TextBatch>("result");
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), 3u);
  EXPECT_EQ((*output)[0].req_id, 7u);
  EXPECT_EQ((*output)[0].sub_id, 8u);
  EXPECT_EQ((*output)[0].data, "P8|C1;C3|route (VIP), word|D1;D2|{literal}");
  EXPECT_EQ((*output)[1].req_id, 2u);
  EXPECT_EQ((*output)[1].sub_id, 9u);
  EXPECT_EQ((*output)[1].data, "P2|C2|other|D3|{literal}");
  EXPECT_EQ((*output)[2].sub_id, 3u);
  EXPECT_EQ((*output)[2].data, "P3|C1;C3|route (VIP), word|D1;D2|{literal}");
}

TEST(TextTemplateNodeTest,
     AggregatedInputsWithoutPrimaryProduceOneSamplePerRequest) {
  SessionContext session;
  NodeProbe node;
  ASSERT_TRUE(node.Init(
      "text_template", {{"template", "{{context}}"}, {"separator", "/"}},
      session, {{"context", "context", ""}}, {{"text", "result", ""}}));
  AlgContext context;
  ASSERT_TRUE(context.Publish("context",
                              RankedTextBatch{{7, 9, RankedCandidate("a")},
                                              {2, 3, RankedCandidate("b")},
                                              {7, 4, RankedCandidate("c")}}));
  ASSERT_EQ(node.node->Process(&context), 0);
  const auto* output = context.Read<TextBatch>("result");
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), 2u);
  EXPECT_EQ((*output)[0].req_id, 2u);
  EXPECT_EQ((*output)[0].sub_id, 0u);
  EXPECT_EQ((*output)[0].data, "b");
  EXPECT_EQ((*output)[1].req_id, 7u);
  EXPECT_EQ((*output)[1].sub_id, 0u);
  EXPECT_EQ((*output)[1].data, "a/c");
}

TEST(TextTemplateNodeTest, TruncatesByUtf8BytesWithoutSplittingACharacter) {
  struct Case {
    int limit;
    const char* expected;
  };
  for (const auto& example : std::vector<Case>{
           {1, "A"}, {3, "A"}, {4, "A猫"}, {7, "A猫"}, {8, "A猫🙂"}}) {
    SCOPED_TRACE(example.limit);
    SessionContext session;
    NodeProbe node;
    ASSERT_TRUE(node.Init(
        "text_template",
        {{"max_length", example.limit}, {"overflow_policy", "truncate"}},
        session, {{"primary", "input", ""}}, {{"text", "result", ""}}));
    AlgContext context;
    ASSERT_TRUE(context.Publish("input", TextBatch{{8, 2, "A猫🙂Z"}}));
    ASSERT_EQ(node.node->Process(&context), 0);
    const auto* output = context.Read<TextBatch>("result");
    ASSERT_NE(output, nullptr);
    ASSERT_EQ(output->size(), 1u);
    EXPECT_EQ(output->front().data, example.expected);
    EXPECT_EQ(output->front().req_id, 8u);
    EXPECT_EQ(output->front().sub_id, 2u);
  }
}

TEST(TextTemplateNodeTest,
     OverflowFailureAndInvalidUtf8PublishNoRenderedPrefix) {
  SessionContext session;
  NodeProbe node;
  ASSERT_TRUE(node.Init("text_template", {{"max_length", 4}}, session,
                        {{"primary", "input", ""}}, {{"text", "result", ""}}));
  AlgContext context;
  ASSERT_TRUE(
      context.Publish("input", TextBatch{{1, 0, "ok"}, {1, 1, "A猫🙂"}}));
  EXPECT_NE(node.node->Process(&context), 0);
  EXPECT_FALSE(context.Has("result"));
  NodeProbe invalid;
  ASSERT_TRUE(invalid.Init(
      "text_template", {{"max_length", 1}, {"overflow_policy", "truncate"}},
      session, {{"primary", "input", ""}}, {{"text", "result", ""}}));
  AlgContext malformed;
  ASSERT_TRUE(
      malformed.Publish("input", TextBatch{{1, 0, std::string("A\xff", 2)}}));
  EXPECT_NE(invalid.node->Process(&malformed), 0);
  EXPECT_FALSE(malformed.Has("result"));
}

TEST(TextTemplateNodeTest,
     ControlRecompilesTemplateAndFailedCompilationKeepsOldState) {
  SessionContext session;
  NodeProbe node;
  ASSERT_TRUE(node.Init("text_template", {{"template", "old={{primary}}"}},
                        session, {{"primary", "input", ""}},
                        {{"text", "result", ""}}));
  const auto rejected = node.node->Control(kControlCmdUpdatePrompt,
                                           R"({"template":"{{context}}"})");
  EXPECT_EQ(rejected.status, NodeControlStatus::kFailed);
  AlgContext old;
  ASSERT_TRUE(old.Publish("input", TextBatch{{3, 4, "Q"}}));
  ASSERT_EQ(node.node->Process(&old), 0);
  EXPECT_EQ(old.Read<TextBatch>("result")->front().data, "old=Q");
  ASSERT_EQ(node.node
                ->Control(kControlCmdUpdatePrompt,
                          R"({"template":"new={{primary}} / {{primary}}"})")
                .status,
            NodeControlStatus::kHandled);
  AlgContext current;
  ASSERT_TRUE(current.Publish("input", TextBatch{{3, 4, "Q"}}));
  ASSERT_EQ(node.node->Process(&current), 0);
  EXPECT_EQ(current.Read<TextBatch>("result")->front().data, "new=Q / Q");
}
}  // namespace
}  // namespace llm_edgeflow

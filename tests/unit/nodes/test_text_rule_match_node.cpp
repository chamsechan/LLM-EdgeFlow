#include <gtest/gtest.h>

#include "contracts/error_codes.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "tests/support/node_probe.h"

namespace llm_edgeflow {
namespace {
using Json = nlohmann::json;
using test_support::NodeProbe;

TEST(TextRuleMatchNodeTest,
     CollectsKeywordAndRuleHitsButFirstCategoryOwnsScalarResult) {
  SessionContext session;
  NodeProbe node;
  ASSERT_TRUE(node.Init("text_rule_match",
                        {{"categories", {{"priority", {"VIP", ""}}}},
                         {"rules",
                          {{{"id", "contains"},
                            {"pattern", "IP"},
                            {"category", "other"},
                            {"score", 0.4}},
                           {{"id", "exact"},
                            {"pattern", "VIP"},
                            {"strategy", "exact"},
                            {"category", "exact"}}}}},
                        session, {{"text", "input", ""}},
                        {{"matches", "result", ""}}))
      << node.diagnostic;
  AlgContext context;
  ASSERT_TRUE(
      context.Publish("input", TextBatch{{9, 7, "VIP"}, {2, 3, "prefixVIP"}}));
  ASSERT_EQ(node.node->Process(&context), 0);
  const auto* matches = context.Read<RuleMatchBatch>("result");
  ASSERT_NE(matches, nullptr);
  ASSERT_EQ(matches->size(), 2u);
  const auto& first = matches->front();
  EXPECT_EQ(first.req_id, 9u);
  EXPECT_EQ(first.sub_id, 7u);
  EXPECT_EQ(first.data.is_hit, 1);
  EXPECT_EQ(first.data.category, "priority");
  EXPECT_EQ(first.data.matched_word, "VIP");
  EXPECT_FLOAT_EQ(first.data.score, 1);
  EXPECT_TRUE(first.data.rule_id.empty());
  ASSERT_EQ(first.data.matches.size(), 3u);
  EXPECT_EQ(first.data.matches[0].source, RuleMatchSource::kKeyword);
  EXPECT_EQ(first.data.matches[1].rule_id, "contains");
  EXPECT_EQ(first.data.matches[2].rule_id, "exact");
  EXPECT_EQ((*matches)[1].data.matches.size(), 2u);
}

TEST(TextRuleMatchNodeTest,
     UnicodeLookbehindCapturesAndTypedConstantsMergeIntoSlots) {
  SessionContext session;
  NodeProbe node;
  ASSERT_TRUE(node.Init(
      "text_rule_match",
      {{"rules",
        {{{"id", "uncategorized"}, {"pattern", "编号"}},
         {{"id", "order"},
          {"strategy", "regex"},
          {"pattern", "(?<=编号=)(?<word>\\p{L}+)(?<number>\\d+)"},
          {"category", "order"},
          {"score", 0.8},
          {"constants",
           {{"number", 99},
            {"route", Json::array({"a", "b"})},
            {"verified", true}}}},
         {{"id", "later"}, {"pattern", "猫"}, {"category", "later"}}}}},
      session, {{"text", "input", ""}}, {{"matches", "result", ""}}))
      << node.diagnostic;
  AlgContext context;
  ASSERT_TRUE(context.Publish("input", TextBatch{{5, 11, "编号=猫7"}}));
  ASSERT_EQ(node.node->Process(&context), 0);
  const auto& result = context.Read<RuleMatchBatch>("result")->front().data;
  EXPECT_EQ(result.category, "order");
  EXPECT_EQ(result.rule_id, "order");
  EXPECT_FLOAT_EQ(result.score, 0.8f);
  EXPECT_EQ(result.slots, Json({{"word", "猫"},
                                {"number", 99},
                                {"route", Json::array({"a", "b"})},
                                {"verified", true}}));
  ASSERT_EQ(result.matches.size(), 3u);
  EXPECT_EQ(result.matches[0].category, "");
  EXPECT_EQ(result.matches[1].source, RuleMatchSource::kRule);
  EXPECT_EQ(result.matches[2].category, "later");
}

TEST(TextRuleMatchNodeTest, ExactMatchingAndDefaultFallbackKeepRawQuery) {
  SessionContext session;
  NodeProbe node;
  ASSERT_TRUE(node.Init("text_rule_match",
                        {{"default_category", "fallback"},
                         {"default_score", 0.25},
                         {"rules",
                          {{{"id", "exact"},
                            {"strategy", "exact"},
                            {"pattern", "yes"},
                            {"category", "confirmed"}}}}},
                        session, {{"text", "input", ""}},
                        {{"matches", "result", ""}}));
  AlgContext context;
  ASSERT_TRUE(
      context.Publish("input", TextBatch{{1, 1, "yes"}, {1, 2, "yes!"}}));
  ASSERT_EQ(node.node->Process(&context), 0);
  const auto* results = context.Read<RuleMatchBatch>("result");
  ASSERT_NE(results, nullptr);
  ASSERT_EQ(results->size(), 2u);
  EXPECT_EQ((*results)[0].data.category, "confirmed");
  EXPECT_EQ((*results)[1].data.category, "fallback");
  EXPECT_EQ((*results)[1].data.is_hit, 1);
  EXPECT_FLOAT_EQ((*results)[1].data.score, 0.25f);
  EXPECT_EQ((*results)[1].data.slots, Json({{"raw_query", "yes!"}}));
  EXPECT_TRUE((*results)[1].data.matches.empty());
  EXPECT_TRUE((*results)[1].data.matched_word.empty());
}

TEST(TextRuleMatchNodeTest,
     InvalidRegexControlPreservesCompiledRulesAndValidControlReplacesThem) {
  SessionContext session;
  NodeProbe node;
  ASSERT_TRUE(node.Init("text_rule_match", {{"categories", {{"old", {"old"}}}}},
                        session, {{"text", "input", ""}},
                        {{"matches", "result", ""}}));
  const auto rejected = node.node->Control(
      kControlCmdUpdateRules,
      R"({"rules":[{"id":"broken","strategy":"regex","pattern":"["}]})");
  EXPECT_EQ(rejected.status, NodeControlStatus::kFailed);
  EXPECT_FALSE(rejected.message.empty());
  AlgContext old;
  ASSERT_TRUE(old.Publish("input", TextBatch{{1, 0, "old"}}));
  ASSERT_EQ(node.node->Process(&old), 0);
  EXPECT_EQ(old.Read<RuleMatchBatch>("result")->front().data.category, "old");
  const auto updated = node.node->Control(
      kControlCmdUpdateRules, R"({"categories":{"new":["new"]},"rules":[]})");
  ASSERT_EQ(updated.status, NodeControlStatus::kHandled);
  AlgContext current;
  ASSERT_TRUE(
      current.Publish("input", TextBatch{{1, 0, "old"}, {1, 1, "new"}}));
  ASSERT_EQ(node.node->Process(&current), 0);
  const auto* output = current.Read<RuleMatchBatch>("result");
  EXPECT_EQ((*output)[0].data.is_hit, 0);
  EXPECT_EQ((*output)[1].data.category, "new");
}

TEST(TextRuleMatchNodeTest,
     RegexExecutionFailureAbortsBatchAndNamesFailedItemOnce) {
  SessionContext session;
  NodeProbe node;
  ASSERT_TRUE(node.Init("text_rule_match",
                        {{"rules",
                          {{{"id", "limited"},
                            {"strategy", "regex"},
                            {"pattern", "(*LIMIT_MATCH=1)(?:a|b)+c"}}}}},
                        session, {{"text", "input", ""}},
                        {{"matches", "result", ""}}))
      << node.diagnostic;
  AlgContext context;
  ASSERT_TRUE(
      context.Publish("input", TextBatch{{4, 1, "plain"}, {4, 6, "ababdc"}}));
  EXPECT_EQ(node.node->Process(&context), error::kRegexExecutionFailed);
  EXPECT_FALSE(context.Has("result"));
  const std::string message = context.GetErrorMessage();
  EXPECT_NE(message.find("Regex execution failed for rule 'limited'"),
            std::string::npos);
  const std::string source = "req_id=4, sub_id=6";
  const auto first = message.find(source);
  ASSERT_NE(first, std::string::npos) << message;
  EXPECT_EQ(message.find(source, first + 1), std::string::npos) << message;
}
}  // namespace
}  // namespace llm_edgeflow

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapter/shared_algorithm_runtime.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline_validator.h"
#include "core/session_context.h"
#include "tests/support/node_test_utils.h"

namespace llm_edgeflow {

class TextRuleMatchNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SharedAlgorithmRuntime::GlobalInit(), 0);
    session_ctx_ = std::make_unique<SessionContext>();
  }
  std::unique_ptr<SessionContext> session_ctx_;
};

// 1. Process Keyword and Category Matching
TEST_F(TextRuleMatchNodeTest, ProcessKeywordAndCategoryMatching) {
  auto node = NodeFactory::Instance().Create("TextRuleMatchNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"categories",
                         {{"COMPLAINT", {"退款", "投诉", "差评"}},
                          {"CONSULT", {"如何", "怎么", "咨询"}}}}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch inputs;
  inputs.emplace_back(1, 0, "我要申请退款并投诉服务");
  inputs.emplace_back(2, 0, "请问如何升级会员账号");
  inputs.emplace_back(3, 0, "今天天气真好");
  ctx.Publish("text", inputs);

  EXPECT_EQ(node->Process(&ctx), 0);

  const auto* matches = ctx.Read<RuleMatchBatch>("matches");
  ASSERT_NE(matches, nullptr);
  ASSERT_EQ(matches->size(), 3u);

  EXPECT_EQ((*matches)[0].data.is_hit, 1);
  EXPECT_EQ((*matches)[0].data.category, "COMPLAINT");
  EXPECT_EQ((*matches)[1].data.is_hit, 1);
  EXPECT_EQ((*matches)[1].data.category, "CONSULT");
  EXPECT_EQ((*matches)[2].data.is_hit, 0);
}

TEST_F(TextRuleMatchNodeTest, RejectsInvalidRuleAndDefaultScores) {
  const std::vector<nlohmann::json> invalid_scores = {
      -0.01,
      1.01,
      1e100,
      std::nextafter(1.0, 2.0),
      std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN(),
      "0.5",
      nullptr};
  for (const auto& score : invalid_scores) {
    SCOPED_TRACE(score.dump());
    auto rule_node = NodeFactory::Instance().Create("TextRuleMatchNode");
    EXPECT_FALSE(InitNodeForTest(
        *rule_node, {{"rules", {{{"pattern", "hit"}, {"score", score}}}}},
        session_ctx_.get()));
    auto default_node = NodeFactory::Instance().Create("TextRuleMatchNode");
    EXPECT_FALSE(InitNodeForTest(*default_node, {{"default_score", score}},
                                 session_ctx_.get()));
  }
}

TEST_F(TextRuleMatchNodeTest, NestedDiagnosticsAgreeAcrossAuthoringAndControl) {
  struct InvalidCase {
    nlohmann::json config;
    std::vector<std::string> expected;
  };
  const std::vector<InvalidCase> cases = {
      {{{"categories", {{"VIP", 123}}}}, {"categories", "VIP", "array"}},
      {{{"categories", {{"VIP", {"valid", false}}}}},
       {"categories", "VIP", "Array item 1", "string"}},
      {{{"rules",
         {{{"pattern", "valid"}}, {{"pattern", "bad"}, {"scroe", 1}}}}},
       {"rules", "Array item 1", "scroe"}},
      {{{"categories", {{"NEW", {"replacement"}}}},
        {"rules",
         {{{"pattern", "valid"}},
          {{"id", "broken"}, {"strategy", "regex"}, {"pattern", "("}}}}},
       {"rules[1].pattern", "broken", "byte offset"}}};
  auto root = nlohmann::json::parse(R"({
    "biz_name":"keyword_match_v1", "models":[], "pipeline":[
      {"id":"rules", "node_type":"TextRuleMatchNode", "depends_on":[],
       "ports":{"inputs":{"text":"input_sentences"},
                "outputs":{"matches":"rule_matches"}}}
    ]})");
  auto active = NodeFactory::Instance().Create("TextRuleMatchNode");
  ASSERT_NE(active, nullptr);
  ASSERT_TRUE(InitNodeForTest(*active, {{"categories", {{"OLD", {"kept"}}}}},
                              session_ctx_.get()));
  for (const auto& invalid : cases) {
    SCOPED_TRACE(invalid.config.dump());
    root["pipeline"][0]["config"] = invalid.config;
    const auto report = PipelineValidator::Validate(root);
    ASSERT_FALSE(report.ok);
    EXPECT_TRUE(std::any_of(
        report.diagnostics.begin(), report.diagnostics.end(),
        [&](const auto& diagnostic) {
          return diagnostic.path == "/pipeline/0/config" &&
                 std::all_of(invalid.expected.begin(), invalid.expected.end(),
                             [&](const auto& part) {
                               return diagnostic.message.find(part) !=
                                      std::string::npos;
                             });
        }))
        << report.ToJson().dump(2);

    // Direct authoring initialization reports the same offending nested value.
    auto fresh = NodeFactory::Instance().Create("TextRuleMatchNode");
    std::string diagnostic;
    NodeInitContext init;
    init.config = &invalid.config;
    init.session_ctx = session_ctx_.get();
    init.diagnostic = &diagnostic;
    EXPECT_FALSE(fresh->Init(init));
    const auto update =
        active->Control(kControlCmdUpdateRules, invalid.config.dump());
    EXPECT_EQ(update.status, NodeControlStatus::kFailed);
    for (const auto& part : invalid.expected) {
      EXPECT_NE(diagnostic.find(part), std::string::npos) << diagnostic;
      EXPECT_NE(update.message.find(part), std::string::npos) << update.message;
    }
    AlgContext context;
    context.Publish("text", TextBatch{{17, 0, "kept"}, {18, 0, "replacement"}});
    ASSERT_EQ(active->Process(&context), 0);
    const auto* matches = context.Read<RuleMatchBatch>("matches");
    ASSERT_NE(matches, nullptr);
    ASSERT_EQ(matches->size(), 2u);
    EXPECT_EQ(matches->at(0).data.category, "OLD");
    EXPECT_EQ(matches->at(1).data.is_hit, 0);
  }
}

TEST_F(TextRuleMatchNodeTest, DirectInitReportsInvalidTopLevelField) {
  for (const auto& [config, field] :
       std::vector<std::pair<nlohmann::json, std::string>>{
           {{{"default_score", "bad"}}, "default_score"},
           {{{"category", "misspelled"}}, "category"}}) {
    auto node = NodeFactory::Instance().Create("TextRuleMatchNode");
    std::string diagnostic;
    NodeInitContext init;
    init.config = &config;
    init.session_ctx = session_ctx_.get();
    init.diagnostic = &diagnostic;
    EXPECT_FALSE(node->Init(init));
    EXPECT_NE(diagnostic.find(field), std::string::npos) << diagnostic;
  }
}

TEST_F(TextRuleMatchNodeTest, ScoreBoundsAndDefaultsRemainUsable) {
  auto node = NodeFactory::Instance().Create("TextRuleMatchNode");
  ASSERT_TRUE(InitNodeForTest(
      *node,
      {{"default_category", "FALLBACK"},
       {"default_score", 0.25},
       {"rules",
        {{{"pattern", "zero"}, {"category", "ZERO"}, {"score", 0}},
         {{"pattern", "one"}, {"category", "ONE"}, {"score", 1}},
         {{"pattern", "default"}, {"category", "DEFAULT"}}}}},
      session_ctx_.get()));
  AlgContext ctx;
  ctx.Publish(
      "text",
      TextBatch{
          {1, 0, "zero"}, {2, 0, "one"}, {3, 0, "default"}, {4, 0, "miss"}});
  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* matches = ctx.Read<RuleMatchBatch>("matches");
  ASSERT_NE(matches, nullptr);
  ASSERT_EQ(matches->size(), 4u);
  EXPECT_FLOAT_EQ(matches->at(0).data.score, 0.0f);
  EXPECT_FLOAT_EQ(matches->at(1).data.score, 1.0f);
  EXPECT_FLOAT_EQ(matches->at(2).data.score, 1.0f);
  EXPECT_FLOAT_EQ(matches->at(3).data.score, 0.25f);
  for (const auto& match : *matches) {
    EXPECT_TRUE(std::isfinite(match.data.score));
    EXPECT_TRUE(match.data.details.at("confidence").is_number());
  }
}

TEST_F(TextRuleMatchNodeTest, InvalidScoreControlPreservesCategoriesAndRules) {
  auto node = NodeFactory::Instance().Create("TextRuleMatchNode");
  ASSERT_TRUE(InitNodeForTest(
      *node,
      {{"categories", {{"OLD", {"kept"}}}},
       {"rules",
        {{{"pattern", "original"}, {"category", "RULE"}, {"score", 0.75}}}}},
      session_ctx_.get()));
  for (const double score : {-0.01, 1.01, 1e100, std::nextafter(1.0, 2.0)}) {
    const nlohmann::json update = {{"categories", {{"NEW", {"replacement"}}}},
                                   {"rules",
                                    {{{"pattern", "replacement"},
                                      {"category", "NEW"},
                                      {"score", score}}}}};
    EXPECT_EQ(node->Control(kControlCmdUpdateRules, update.dump()).status,
              NodeControlStatus::kFailed);
  }
  AlgContext ctx;
  ctx.Publish(
      "text",
      TextBatch{{1, 0, "kept"}, {2, 0, "original"}, {3, 0, "replacement"}});
  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* matches = ctx.Read<RuleMatchBatch>("matches");
  ASSERT_NE(matches, nullptr);
  ASSERT_EQ(matches->size(), 3u);
  EXPECT_EQ(matches->at(0).data.category, "OLD");
  EXPECT_EQ(matches->at(1).data.category, "RULE");
  EXPECT_FLOAT_EQ(matches->at(1).data.score, 0.75f);
  EXPECT_EQ(matches->at(2).data.is_hit, 0);
}

// 2. Control Command Dynamic Rule Hot-Swap & Bogus Rejection
TEST_F(TextRuleMatchNodeTest, ControlCommandDynamicRules) {
  auto node = NodeFactory::Instance().Create("TextRuleMatchNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));

  // Valid update
  nlohmann::json update_payload = {
      {"categories", {{"SECURITY", {"密码", "漏洞", "盗号"}}}}};
  NodeControlResult res =
      node->Control(kControlCmdUpdateRules, update_payload.dump());
  EXPECT_EQ(res.status, NodeControlStatus::kHandled);

  // Bogus update -> Rejected
  nlohmann::json bogus_payload = {{"bogus_field", 123}};
  NodeControlResult bogus_res =
      node->Control(kControlCmdUpdateRules, bogus_payload.dump());
  EXPECT_EQ(bogus_res.status, NodeControlStatus::kFailed);
}

TEST_F(TextRuleMatchNodeTest, CombinedControlUpdateIsAtomic) {
  auto node = NodeFactory::Instance().Create("TextRuleMatchNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));

  const nlohmann::json combined_update = {
      {"categories", {{"SECURITY", {"密码"}}}},
      {"rules",
       {{{"id", "breach"},
         {"strategy", "contains"},
         {"pattern", "breach"},
         {"category", "INCIDENT"}}}}};
  ASSERT_EQ(
      node->Control(kControlCmdUpdateRules, combined_update.dump()).status,
      NodeControlStatus::kHandled);

  AlgContext updated_ctx;
  TextBatch updated_inputs;
  updated_inputs.emplace_back(1, 0, "密码泄露");
  updated_inputs.emplace_back(2, 0, "data breach");
  updated_ctx.Publish("text", std::move(updated_inputs));
  ASSERT_EQ(node->Process(&updated_ctx), 0);
  const auto* updated = updated_ctx.Read<RuleMatchBatch>("matches");
  ASSERT_NE(updated, nullptr);
  ASSERT_EQ(updated->size(), 2u);
  EXPECT_EQ((*updated)[0].data.category, "SECURITY");
  EXPECT_EQ((*updated)[1].data.category, "INCIDENT");
  EXPECT_EQ((*updated)[1].data.rule_id, "breach");

  const nlohmann::json invalid_combined_update = {
      {"categories", {{"REPLACED", {"new"}}}},
      {"rules",
       {{{"id", "invalid"},
         {"strategy", "regex"},
         {"pattern", R"((?<=a+)b)"},
         {"category", "INVALID"}}}}};
  EXPECT_EQ(
      node->Control(kControlCmdUpdateRules, invalid_combined_update.dump())
          .status,
      NodeControlStatus::kFailed);

  AlgContext preserved_ctx;
  TextBatch preserved_inputs;
  preserved_inputs.emplace_back(3, 0, "密码泄露");
  preserved_inputs.emplace_back(4, 0, "data breach");
  preserved_inputs.emplace_back(5, 0, "new");
  preserved_ctx.Publish("text", std::move(preserved_inputs));
  ASSERT_EQ(node->Process(&preserved_ctx), 0);
  const auto* preserved = preserved_ctx.Read<RuleMatchBatch>("matches");
  ASSERT_NE(preserved, nullptr);
  ASSERT_EQ(preserved->size(), 3u);
  EXPECT_EQ((*preserved)[0].data.category, "SECURITY");
  EXPECT_EQ((*preserved)[1].data.category, "INCIDENT");
  EXPECT_EQ((*preserved)[2].data.is_hit, 0);
}

TEST_F(TextRuleMatchNodeTest, MisspelledRuleCannotBecomeMatchAll) {
  auto node = NodeFactory::Instance().Create("TextRuleMatchNode");
  ASSERT_TRUE(InitNodeForTest(*node, {{"categories", {{"OLD", {"kept"}}}}},
                              session_ctx_.get()));
  const nlohmann::json invalid = {{"categories", {{"NEW", {"unrelated"}}}},
                                  {"rules",
                                   {{{"strategy", "contains"},
                                     {"patern", "never"},
                                     {"category", "WRONG"}}}}};
  EXPECT_EQ(node->Control(kControlCmdUpdateRules, invalid.dump()).status,
            NodeControlStatus::kFailed);
  AlgContext ctx;
  TextBatch input;
  input.emplace_back(1, 0, "kept");
  input.emplace_back(2, 0, "unrelated");
  ctx.Publish("text", std::move(input));
  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* output = ctx.Read<RuleMatchBatch>("matches");
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), 2u);
  EXPECT_EQ(output->at(0).data.category, "OLD");
  EXPECT_EQ(output->at(1).data.is_hit, 0);
}

TEST_F(TextRuleMatchNodeTest, SupportsLookbehindAndNamedCaptures) {
  auto node = NodeFactory::Instance().Create("TextRuleMatchNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"rules",
                         {{{"id", "amount"},
                           {"strategy", "regex"},
                           {"pattern", R"((?<=金额:)(?P<amount>\d+))"},
                           {"category", "AMOUNT"}},
                          {{"id", "risk"},
                           {"strategy", "regex"},
                           {"pattern", R"((?<!not_)(?<word>risk))"},
                           {"category", "RISK"}}}}};
  ASSERT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch inputs;
  inputs.emplace_back(11, 1, "本次金额:123元");
  inputs.emplace_back(12, 2, "not_risk");
  inputs.emplace_back(13, 3, "plain risk");
  ctx.Publish("text", inputs);

  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* matches = ctx.Read<RuleMatchBatch>("matches");
  ASSERT_NE(matches, nullptr);
  ASSERT_EQ(matches->size(), 3u);

  EXPECT_EQ((*matches)[0].req_id, 11u);
  EXPECT_EQ((*matches)[0].sub_id, 1u);
  EXPECT_EQ((*matches)[0].data.category, "AMOUNT");
  EXPECT_EQ((*matches)[0].data.captures.at("amount"), "123");
  EXPECT_EQ((*matches)[1].data.is_hit, 0);
  EXPECT_EQ((*matches)[2].data.category, "RISK");
  EXPECT_EQ((*matches)[2].data.captures.at("word"), "risk");
}

TEST_F(TextRuleMatchNodeTest, PreservesLookbehindWhenLaterGreaterThanExists) {
  auto node = NodeFactory::Instance().Create("TextRuleMatchNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"rules",
                         {{{"id", "regression"},
                           {"strategy", "regex"},
                           {"pattern", R"((?<=不存在)(?<value>请帮我>联系))"},
                           {"category", "RISK"}}}}};
  ASSERT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch inputs;
  inputs.emplace_back(21, 0, "请帮我联系一下");
  inputs.emplace_back(22, 0, "不存在请帮我>联系");
  ctx.Publish("text", inputs);

  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* matches = ctx.Read<RuleMatchBatch>("matches");
  ASSERT_NE(matches, nullptr);
  ASSERT_EQ(matches->size(), 2u);
  EXPECT_EQ((*matches)[0].data.is_hit, 0);
  EXPECT_EQ((*matches)[1].data.is_hit, 1);
  EXPECT_EQ((*matches)[1].data.captures.at("value"), "请帮我>联系");
}

TEST_F(TextRuleMatchNodeTest, RegexErrorsFailClosed) {
  auto node = NodeFactory::Instance().Create("TextRuleMatchNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));

  nlohmann::json invalid_update = {{"rules",
                                    {{{"id", "invalid"},
                                      {"strategy", "regex"},
                                      {"pattern", R"((?<=a+)b)"},
                                      {"category", "INVALID"}}}}};
  EXPECT_EQ(node->Control(kControlCmdUpdateRules, invalid_update.dump()).status,
            NodeControlStatus::kFailed);

  nlohmann::json valid_update = {{"rules",
                                  {{{"id", "utf8"},
                                    {"strategy", "regex"},
                                    {"pattern", "."},
                                    {"category", "ANY"}}}}};
  ASSERT_EQ(node->Control(kControlCmdUpdateRules, valid_update.dump()).status,
            NodeControlStatus::kHandled);

  AlgContext ctx;
  TextBatch inputs;
  inputs.emplace_back(31, 0, std::string("ok") + "\xE4\xB8");
  ctx.Publish("text", inputs);
  EXPECT_EQ(node->Process(&ctx), -5002);
  EXPECT_EQ(ctx.Read<RuleMatchBatch>("matches"), nullptr);
}

// 3. Missing Input Fails Closed
TEST_F(TextRuleMatchNodeTest, MissingInputFailsClosed) {
  auto node = NodeFactory::Instance().Create("TextRuleMatchNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_EQ(node->Process(&empty_ctx), -5001);
}

}  // namespace llm_edgeflow

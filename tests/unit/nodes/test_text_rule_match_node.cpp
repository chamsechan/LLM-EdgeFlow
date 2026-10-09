#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline_validator.h"
#include "core/session_context.h"
#include "nodes/node_error_codes.h"
#include "tests/support/node_process_pause.h"
#include "tests/support/node_test_utils.h"
#include "tests/support/pipeline_test_utils.h"
#include "tests/support/scoped_allocation_failure.h"

namespace llm_edgeflow {

class TextRuleMatchNodeTest : public ::testing::Test {
 protected:
  void SetUp() override { session_ctx_ = std::make_unique<SessionContext>(); }
  std::unique_ptr<SessionContext> session_ctx_;
};

TEST_F(TextRuleMatchNodeTest, ProcessKeywordAndCategoryMatching) {
  auto node = NodeRegistry::Instance().Create("text_rule_match");
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
    auto rule_node = NodeRegistry::Instance().Create("text_rule_match");
    EXPECT_FALSE(InitNodeForTest(
        *rule_node, {{"rules", {{{"pattern", "hit"}, {"score", score}}}}},
        session_ctx_.get()));
    auto default_node = NodeRegistry::Instance().Create("text_rule_match");
    EXPECT_FALSE(InitNodeForTest(*default_node, {{"default_score", score}},
                                 session_ctx_.get()));
  }
}

TEST_F(TextRuleMatchNodeTest, NestedDiagnosticsAgreeAcrossAuthoringAndControl) {
  struct InvalidCase {
    nlohmann::json config;
    std::string path;
    DiagnosticCode code;
    std::vector<std::string> expected;
  };
  const std::vector<InvalidCase> cases = {
      {{{"categories", {{"VIP", 123}}}},
       "/pipeline/0/params/categories/VIP",
       DiagnosticCode::kConfigFieldType,
       {"array"}},
      {{{"categories", {{"VIP", {"valid", false}}}}},
       "/pipeline/0/params/categories/VIP/1",
       DiagnosticCode::kConfigFieldType,
       {"string"}},
      {{{"rules",
         {{{"pattern", "valid"}}, {{"pattern", "bad"}, {"scroe", 1}}}}},
       "/pipeline/0/params/rules/1/scroe",
       DiagnosticCode::kUnknownConfigField,
       {"scroe"}},
      {{{"rules", {{{"pattern", "valid"}}, {{"pattern", 7}}}}},
       "/pipeline/0/params/rules/1/pattern",
       DiagnosticCode::kConfigFieldType,
       {"string"}},
      {{{"rules", {{{"pattern", "valid"}}, {{"category", "MISSING"}}}}},
       "/pipeline/0/params/rules/1/pattern",
       DiagnosticCode::kMissingConfigField,
       {"pattern"}},
      {{{"categories", {{"NEW", {"replacement"}}}},
        {"rules",
         {{{"pattern", "valid"}},
          {{"id", "broken"}, {"strategy", "regex"}, {"pattern", "("}}}}},
       "/pipeline/0/params/rules",
       DiagnosticCode::kInvalidCombination,
       {"Field 'rules'", "Array item 1", "pattern", "broken", "byte offset"}}};
  auto root = nlohmann::json::parse(R"({
  "models": [],
  "pipeline": [
    {
      "type": "text_rule_match",
      "name": "rules",
      "inputs": {
        "text": "input.input_sentences"
      }
    }
  ]
})");
  auto active = NodeRegistry::Instance().Create("text_rule_match");
  ASSERT_NE(active, nullptr);
  ASSERT_TRUE(InitNodeForTest(*active, {{"categories", {{"OLD", {"kept"}}}}},
                              session_ctx_.get()));
  for (const auto& invalid : cases) {
    SCOPED_TRACE(invalid.config.dump());
    root["pipeline"][0]["params"] = invalid.config;
    const auto report = PipelineValidator::Validate(
        root, MakeTestBoundary({{"input.input_sentences", "TextBatch"}},
                               {{"rules.matches", "RuleMatchBatch"}}));
    ASSERT_FALSE(report.ok);
    EXPECT_TRUE(std::any_of(
        report.diagnostics.begin(), report.diagnostics.end(),
        [&](const auto& diagnostic) {
          return diagnostic.path == invalid.path &&
                 diagnostic.code == invalid.code &&
                 std::all_of(invalid.expected.begin(), invalid.expected.end(),
                             [&](const auto& part) {
                               return diagnostic.message.find(part) !=
                                      std::string::npos;
                             });
        }))
        << report.ToJson().dump(2);

    const auto update =
        active->Control(kControlCmdUpdateRules, invalid.config.dump());
    EXPECT_EQ(update.status, NodeControlStatus::kFailed);
    for (const auto& part : invalid.expected) {
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

TEST_F(TextRuleMatchNodeTest, ScoreBoundsAndDefaultsRemainUsable) {
  auto node = NodeRegistry::Instance().Create("text_rule_match");
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
  }
}

TEST_F(TextRuleMatchNodeTest, MatchingOrderAndJsonConstantsRemainStable) {
  auto node = NodeRegistry::Instance().Create("text_rule_match");
  ASSERT_NE(node, nullptr);
  const nlohmann::json constants = {
      {"target", "override"},
      {"integer", 7},
      {"number", 2.5},
      {"boolean", true},
      {"array", nlohmann::json::array({1, "two", false})},
      {"object", {{"enabled", true}}}};
  ASSERT_TRUE(InitNodeForTest(
      *node,
      {{"categories", {{"Z_LAST", {"go"}}, {"A_FIRST", {"go"}}}},
       {"rules",
        {{{"id", "z_first"},
          {"strategy", "regex"},
          {"pattern", "(?<target>go)"},
          {"category", "RULE_FIRST"},
          {"score", 0.4},
          {"constants", constants}},
         {{"id", "a_second"},
          {"pattern", "go"},
          {"category", "RULE_SECOND"}}}}},
      session_ctx_.get()));
  AlgContext context;
  context.Publish("text", TextBatch{{31, 7, "go"}});
  ASSERT_EQ(node->Process(&context), 0);
  const auto* matches = context.Read<RuleMatchBatch>("matches");
  ASSERT_NE(matches, nullptr);
  ASSERT_EQ(matches->size(), 1u);
  EXPECT_EQ(matches->front().req_id, 31u);
  EXPECT_EQ(matches->front().sub_id, 7u);
  const auto& match = matches->front().data;
  EXPECT_EQ(match.category, "A_FIRST");
  ASSERT_EQ(match.matches.size(), 4u);
  EXPECT_EQ(match.matches[0].category, "A_FIRST");
  EXPECT_EQ(match.matches[1].category, "Z_LAST");
  EXPECT_EQ(match.matches[2].rule_id, "z_first");
  EXPECT_EQ(match.matches[3].rule_id, "a_second");
  EXPECT_FLOAT_EQ(match.matches[3].score, 1.0f);
  EXPECT_EQ(match.slots, constants);
}

TEST_F(TextRuleMatchNodeTest, EmptyRegexRemainsUnmatched) {
  auto node = NodeRegistry::Instance().Create("text_rule_match");
  ASSERT_TRUE(InitNodeForTest(
      *node,
      {{"rules",
        {{{"strategy", "regex"}, {"pattern", ""}, {"category", "EMPTY"}}}}},
      session_ctx_.get()));
  AlgContext context;
  context.Publish("text", TextBatch{{1, 4, ""}, {2, 5, "ordinary text"}});
  ASSERT_EQ(node->Process(&context), 0);
  const auto* matches = context.Read<RuleMatchBatch>("matches");
  ASSERT_NE(matches, nullptr);
  ASSERT_EQ(matches->size(), 2u);
  for (const auto& match : *matches) {
    EXPECT_EQ(match.data.is_hit, 0);
    EXPECT_TRUE(match.data.matches.empty());
  }
}

TEST_F(TextRuleMatchNodeTest, PartialControlReplacesOnlyProvidedContainers) {
  auto node = NodeRegistry::Instance().Create("text_rule_match");
  ASSERT_TRUE(InitNodeForTest(
      *node,
      {{"categories", {{"OLD", {"kept"}}}},
       {"rules", {{{"pattern", "original"}, {"category", "OLD_RULE"}}}},
       {"default_category", "FALLBACK"},
       {"default_score", 0.25}},
      session_ctx_.get()));
  ASSERT_EQ(node->Control(kControlCmdUpdateRules,
                          R"({"categories":{"NEW":["replacement"]}})")
                .status,
            NodeControlStatus::kHandled);
  AlgContext categories_updated;
  categories_updated.Publish(
      "text",
      TextBatch{{1, 0, "kept"}, {2, 0, "replacement"}, {3, 0, "original"}});
  ASSERT_EQ(node->Process(&categories_updated), 0);
  const auto* before = categories_updated.Read<RuleMatchBatch>("matches");
  ASSERT_NE(before, nullptr);
  ASSERT_EQ(before->size(), 3u);
  EXPECT_EQ(before->at(0).data.category, "FALLBACK");
  EXPECT_FLOAT_EQ(before->at(0).data.score, 0.25f);
  EXPECT_EQ(before->at(1).data.category, "NEW");
  EXPECT_EQ(before->at(2).data.category, "OLD_RULE");

  ASSERT_EQ(
      node->Control(
              kControlCmdUpdateRules,
              R"({"rules":[{"pattern":"updated","category":"NEW_RULE"}]})")
          .status,
      NodeControlStatus::kHandled);
  EXPECT_EQ(node->Control(kControlCmdUpdateRules, "{}").status,
            NodeControlStatus::kFailed);
  AlgContext rules_updated;
  rules_updated.Publish(
      "text",
      TextBatch{{1, 0, "replacement"}, {2, 0, "original"}, {3, 0, "updated"}});
  ASSERT_EQ(node->Process(&rules_updated), 0);
  const auto* after = rules_updated.Read<RuleMatchBatch>("matches");
  ASSERT_NE(after, nullptr);
  ASSERT_EQ(after->size(), 3u);
  EXPECT_EQ(after->at(0).data.category, "NEW");
  EXPECT_EQ(after->at(1).data.category, "FALLBACK");
  EXPECT_FLOAT_EQ(after->at(1).data.score, 0.25f);
  EXPECT_EQ(after->at(2).data.category, "NEW_RULE");
}

TEST_F(TextRuleMatchNodeTest, InvalidScoreControlPreservesCategoriesAndRules) {
  auto node = NodeRegistry::Instance().Create("text_rule_match");
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

// Control 命令动态热替换规则，并拒绝无效更新
TEST_F(TextRuleMatchNodeTest, ControlCommandDynamicRules) {
  auto node = NodeRegistry::Instance().Create("text_rule_match");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));

  // 合法更新
  nlohmann::json update_payload = {
      {"categories", {{"SECURITY", {"密码", "漏洞", "盗号"}}}}};
  NodeControlResult res =
      node->Control(kControlCmdUpdateRules, update_payload.dump());
  EXPECT_EQ(res.status, NodeControlStatus::kHandled);

  // 无效更新 -> 拒绝
  nlohmann::json bogus_payload = {{"bogus_field", 123}};
  NodeControlResult bogus_res =
      node->Control(kControlCmdUpdateRules, bogus_payload.dump());
  EXPECT_EQ(bogus_res.status, NodeControlStatus::kFailed);
}

TEST_F(TextRuleMatchNodeTest, CombinedControlUpdateIsAtomic) {
  auto node = NodeRegistry::Instance().Create("text_rule_match");
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
  auto node = NodeRegistry::Instance().Create("text_rule_match");
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
  auto node = NodeRegistry::Instance().Create("text_rule_match");
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
  EXPECT_EQ((*matches)[0].data.slots["amount"], "123");
  EXPECT_EQ((*matches)[1].data.is_hit, 0);
  EXPECT_EQ((*matches)[2].data.category, "RISK");
  EXPECT_EQ((*matches)[2].data.slots["word"], "risk");
}

TEST_F(TextRuleMatchNodeTest, PreservesLookbehindWhenLaterGreaterThanExists) {
  auto node = NodeRegistry::Instance().Create("text_rule_match");
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
  EXPECT_EQ((*matches)[1].data.slots["value"], "请帮我>联系");
}

TEST_F(TextRuleMatchNodeTest, RegexErrorsFailClosed) {
  auto node = NodeRegistry::Instance().Create("text_rule_match");
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

TEST_F(TextRuleMatchNodeTest, MissingInputFailsClosed) {
  auto node = NodeRegistry::Instance().Create("text_rule_match");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_EQ(node->Process(&empty_ctx), node_error::author_node::kMissingInput);
}

TEST_F(TextRuleMatchNodeTest, DirectConcurrentProcessAndControl) {
  auto node = NodeRegistry::Instance().Create("text_rule_match");
  ASSERT_NE(node, nullptr);
  auto configuration = [](const std::string& version) {
    return nlohmann::json{{"categories", {{version, {"hello"}}}},
                          {"rules",
                           {{{"id", version + "_rule"},
                             {"strategy", "regex"},
                             {"pattern", "(?<" + version + ">world)"},
                             {"category", version}}}}};
  };
  ASSERT_TRUE(InitNodeForTest(*node, configuration("OLD"), session_ctx_.get()));
  const auto update = configuration("NEW");
  // 类别关键词和已编译的正则捕获都能区分版本。
  const TextBatch inputs{{101, 2, "hello"},
                         {101, 7, "world"},
                         {202, 3, "hello"},
                         {202, 8, "world"}};
  AlgContext in_flight;
  in_flight.Publish("text", inputs);
  test_support::NodeProcessPause pause;
  int process_result = -1;
  std::exception_ptr reader_error;
  // 在这条合法的 Process 路径上，snapshot.Read 之后的第一次堆分配是
  // 输出批次的 reserve。在保留旧快照的状态下暂停。
  std::thread reader([&] {
    try {
      test_support::ScopedNextAllocationCallback callback(
          &test_support::NodeProcessPause::OnAllocation, &pause);
      process_result = node->Process(&in_flight);
    } catch (...) {
      reader_error = std::current_exception();
    }
  });
  const bool paused = pause.WaitUntilPaused();
  NodeControlStatus control_status = NodeControlStatus::kFailed;
  std::exception_ptr control_error;
  if (paused) {
    try {
      control_status =
          node->Control(kControlCmdUpdateRules, update.dump()).status;
    } catch (...) {
      control_error = std::current_exception();
    }
  }
  pause.Resume();
  reader.join();
  ASSERT_TRUE(paused) << "Process never reached the snapshot pause";
  ASSERT_EQ(reader_error, nullptr);
  ASSERT_EQ(control_error, nullptr);
  ASSERT_EQ(control_status, NodeControlStatus::kHandled);
  ASSERT_EQ(process_result, 0);

  auto expect_batch = [&](const AlgContext& ctx, const std::string& version) {
    const auto* output = ctx.Read<RuleMatchBatch>("matches");
    ASSERT_NE(output, nullptr);
    ASSERT_EQ(output->size(), inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
      EXPECT_EQ((*output)[i].req_id, inputs[i].req_id);
      EXPECT_EQ((*output)[i].sub_id, inputs[i].sub_id);
      EXPECT_EQ((*output)[i].data.is_hit, 1);
      EXPECT_EQ((*output)[i].data.category, version);
      if (inputs[i].data == "world") {
        EXPECT_EQ((*output)[i].data.rule_id, version + "_rule");
        EXPECT_EQ((*output)[i].data.slots.value(version, ""), "world");
        EXPECT_FALSE(
            (*output)[i].data.slots.contains(version == "OLD" ? "NEW" : "OLD"));
      }
    }
  };
  expect_batch(in_flight, "OLD");
  AlgContext subsequent;
  subsequent.Publish("text", inputs);
  ASSERT_EQ(node->Process(&subsequent), 0);
  expect_batch(subsequent, "NEW");
}

}  // namespace llm_edgeflow

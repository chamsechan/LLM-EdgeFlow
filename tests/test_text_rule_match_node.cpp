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
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch inputs;
  inputs.emplace_back(1, 0, "我要申请退款并投诉服务");
  inputs.emplace_back(2, 0, "请问如何升级会员账号");
  inputs.emplace_back(3, 0, "今天天气真好");
  ctx.Set("text", inputs);

  EXPECT_EQ(node->Process(&ctx), 0);

  const auto* matches = ctx.Get<RuleMatchBatch>("matches");
  ASSERT_NE(matches, nullptr);
  ASSERT_EQ(matches->size(), 3u);

  EXPECT_EQ((*matches)[0].data.is_hit, 1);
  EXPECT_EQ((*matches)[0].data.category, "COMPLAINT");
  EXPECT_EQ((*matches)[1].data.is_hit, 1);
  EXPECT_EQ((*matches)[1].data.category, "CONSULT");
  EXPECT_EQ((*matches)[2].data.is_hit, 0);
}

// 2. Control Command Dynamic Rule Hot-Swap & Bogus Rejection
TEST_F(TextRuleMatchNodeTest, ControlCommandDynamicRules) {
  auto node = NodeFactory::Instance().Create("TextRuleMatchNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(node->Init(nlohmann::json::object(), session_ctx_.get()));

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
  ASSERT_TRUE(node->Init(nlohmann::json::object(), session_ctx_.get()));

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
  updated_ctx.Set("text", std::move(updated_inputs));
  ASSERT_EQ(node->Process(&updated_ctx), 0);
  const auto* updated = updated_ctx.Get<RuleMatchBatch>("matches");
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
  preserved_ctx.Set("text", std::move(preserved_inputs));
  ASSERT_EQ(node->Process(&preserved_ctx), 0);
  const auto* preserved = preserved_ctx.Get<RuleMatchBatch>("matches");
  ASSERT_NE(preserved, nullptr);
  ASSERT_EQ(preserved->size(), 3u);
  EXPECT_EQ((*preserved)[0].data.category, "SECURITY");
  EXPECT_EQ((*preserved)[1].data.category, "INCIDENT");
  EXPECT_EQ((*preserved)[2].data.is_hit, 0);
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
  ASSERT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch inputs;
  inputs.emplace_back(11, 1, "本次金额:123元");
  inputs.emplace_back(12, 2, "not_risk");
  inputs.emplace_back(13, 3, "plain risk");
  ctx.Set("text", inputs);

  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* matches = ctx.Get<RuleMatchBatch>("matches");
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
  ASSERT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch inputs;
  inputs.emplace_back(21, 0, "请帮我联系一下");
  inputs.emplace_back(22, 0, "不存在请帮我>联系");
  ctx.Set("text", inputs);

  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* matches = ctx.Get<RuleMatchBatch>("matches");
  ASSERT_NE(matches, nullptr);
  ASSERT_EQ(matches->size(), 2u);
  EXPECT_EQ((*matches)[0].data.is_hit, 0);
  EXPECT_EQ((*matches)[1].data.is_hit, 1);
  EXPECT_EQ((*matches)[1].data.captures.at("value"), "请帮我>联系");
}

TEST_F(TextRuleMatchNodeTest, RegexErrorsFailClosed) {
  auto node = NodeFactory::Instance().Create("TextRuleMatchNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(node->Init(nlohmann::json::object(), session_ctx_.get()));

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
  ctx.Set("text", inputs);
  EXPECT_EQ(node->Process(&ctx), -5002);
  EXPECT_EQ(ctx.Get<RuleMatchBatch>("matches"), nullptr);
}

// 3. Missing Input Fails Closed
TEST_F(TextRuleMatchNodeTest, MissingInputFailsClosed) {
  auto node = NodeFactory::Instance().Create("TextRuleMatchNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(node->Init(nlohmann::json::object(), session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_EQ(node->Process(&empty_ctx), -5001);
}

}  // namespace alg_framework

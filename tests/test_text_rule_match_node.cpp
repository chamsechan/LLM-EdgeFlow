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

// 3. Missing Input Fails Closed
TEST_F(TextRuleMatchNodeTest, MissingInputFailsClosed) {
  auto node = NodeFactory::Instance().Create("TextRuleMatchNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(node->Init(nlohmann::json::object(), session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_NE(node->Process(&empty_ctx), 0);
}

}  // namespace alg_framework

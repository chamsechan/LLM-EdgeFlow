#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "adapter/biz_adapter_registry.h"
#include "adapter/shared_algorithm_runtime.h"
#include "company_alg_interface.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"

namespace alg_framework {

class AdapterPurityTest : public ::testing::Test {
 protected:
  void SetUp() override { SharedAlgorithmRuntime::GlobalInit(); }
};

// 1. KeywordMatchAdapter Purity
TEST_F(AdapterPurityTest, KeywordMatchAdapterPurity) {
  auto adapter =
      BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_KEYWORD_MATCH);
  ASSERT_NE(adapter, nullptr);

  // Unpack check: C Struct -> AlgContext
  CompanyKeywordInputStruct input{};
  input.request_id = 12345;
  const char* sentence = "测试输入文本";
  input.sentence_text = sentence;
  const void* inputs[] = {&input};

  AlgContext ctx;
  AdapterStatus status;
  ASSERT_EQ(adapter->Unpack(inputs, 1, &ctx, &status), 0);

  const auto* req_ids = ctx.Get(kRawRequestIds);
  const auto* text_batch = ctx.Get(kInputSentences);
  ASSERT_NE(req_ids, nullptr);
  ASSERT_NE(text_batch, nullptr);
  EXPECT_EQ((*req_ids)[0], 12345u);
  EXPECT_EQ((*text_batch)[0].data, sentence);

  // Pack check: AlgContext -> C Struct (Pure transparent copy, zero business
  // synthesis)
  RuleMatchBatch match_batch;
  RuleMatchItem match_item(1, "TEST_CAT", "测试", "{\"intent\":\"TEST_CAT\"}",
                           1.0f);
  match_batch.emplace_back(0, 0, std::move(match_item));
  ctx.Set(kRuleMatches, std::move(match_batch));

  CompanyKeywordOutputStruct output{};
  void* outputs[] = {&output};
  int num_outputs = 1;
  ASSERT_EQ(adapter->Pack(&ctx, outputs, &num_outputs, &status), 0);

  EXPECT_EQ(output.request_id, 12345u);
  EXPECT_EQ(output.is_hit, 1);
  EXPECT_STREQ(output.match_result_json, "{\"intent\":\"TEST_CAT\"}");
}

// 2. ComplianceAuditAdapter Purity
TEST_F(AdapterPurityTest, ComplianceAuditAdapterPurity) {
  auto adapter =
      BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_COMPLIANCE_AUDIT);
  ASSERT_NE(adapter, nullptr);

  CompanyAuditInputStruct in{};
  in.request_id = 8888;
  in.user_text = "客户投诉退款问题";
  in.channel_name = "VIP_HOTLINE";
  const void* inputs[] = {&in};

  AlgContext ctx;
  AdapterStatus status;
  ASSERT_EQ(adapter->Unpack(inputs, 1, &ctx, &status), 0);

  // Pack structured verdict from AlgContext directly
  StructuredDocumentBatch verdicts;
  verdicts.emplace_back(
      0, 0,
      JsonDocumentItem("{\"risk_level\":\"HIGH_RISK\",\"risk_score\":0.92}"));
  ctx.Set(kStructuredVerdicts, std::move(verdicts));

  RankedTextBatch policies;
  policies.emplace_back(0, 0,
                        RankedCandidate("Clause 9.1 Refund Policy", 0.95f, 1));
  ctx.Set(kMatchedPolicy, std::move(policies));

  CompanyAuditOutputStruct out{};
  void* outputs[] = {&out};
  int num_out = 1;
  ASSERT_EQ(adapter->Pack(&ctx, outputs, &num_out, &status), 0);

  EXPECT_EQ(out.request_id, 8888u);
  EXPECT_STREQ(out.risk_level, "HIGH_RISK");
  EXPECT_FLOAT_EQ(out.risk_score, 0.92f);
  EXPECT_STREQ(out.matched_policy_clause, "Clause 9.1 Refund Policy");
}

}  // namespace alg_framework

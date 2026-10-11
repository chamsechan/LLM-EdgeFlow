#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "edgeflow/operator/interface.h"
#include "edgeflow/operator/types.h"
#include "platform_mock/operator_data_types.h"
#include "tests/integration/json_output_test_utils.h"
#include "tests/support/operator_test_fixture.h"

namespace llm_edgeflow {
using test_support::ParseJsonOutput;
using test_support::ScopedTestOperator;

using DialogueAuditOperatorTest = test_support::OperatorTestFixture;

TEST_F(DialogueAuditOperatorTest, RiskAndSafeRequests) {
  ScopedTestOperator instance(ops_);
  ASSERT_EQ(instance.Create("demo/fixtures/mock/pipeline_dialogue_audit.conf"),
            0)
      << instance.create_diagnostic();
  ASSERT_NE(instance.get(), nullptr);

  // 样本 A: 违规诱导私下交易
  std::string s_viol =
      "亲，平台退款审核太慢了，你加我私人微信转账给我吧，我私下把商品寄给你，还"
      "能返现20元！";
  std::string c_viol = "VIP专席客服";

  // 样本 B: 合规正常客服沟通
  std::string s_safe =
      "您好，您的商品符合7天无理由退货政策，已为您"
      "在系统提交退款换货流程，请保持手机畅通。";
  std::string c_safe = "在线售后IM";

  CompanyString cs_viol{static_cast<int32_t>(s_viol.size()),
                        const_cast<char*>(s_viol.data())};
  CompanyString cc_viol{static_cast<int32_t>(c_viol.size()),
                        const_cast<char*>(c_viol.data())};

  CompanyString cs_safe{static_cast<int32_t>(s_safe.size()),
                        const_cast<char*>(s_safe.data())};
  CompanyString cc_safe{static_cast<int32_t>(c_safe.size()),
                        const_cast<char*>(c_safe.data())};

  CompanyOperatorAuditInput req_violation{kMockServiceDialogueAudit, &cs_viol,
                                          &cc_viol};
  CompanyOperatorAuditInput req_safe{kMockServiceDialogueAudit, &cs_safe,
                                     &cc_safe};

  operator_api::NamedIoBatch inputs(2);
  inputs[0]["audit_channel.audit_in"] =
      operator_api::MakeBorrowedOperatorInput(&req_violation);
  inputs[1]["audit_channel.audit_in"] =
      operator_api::MakeBorrowedOperatorInput(&req_safe);

  operator_api::NamedIoBatch outputs(2);
  outputs[0]["audit_channel.json_out"] = nullptr;
  outputs[1]["audit_channel.json_out"] = nullptr;

  const int ret = ops_.Process(instance.get(), inputs, outputs);
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(outputs.size(), 2u);

  auto out_viol_sp = outputs[0]["audit_channel.json_out"];
  auto out_safe_sp = outputs[1]["audit_channel.json_out"];
  ASSERT_NE(out_viol_sp, nullptr);
  ASSERT_NE(out_safe_sp, nullptr);

  auto* out_violation = static_cast<CompanyOperatorOutput*>(out_viol_sp.get());
  const auto out_violation_json = ParseJsonOutput(out_violation);
  auto* out_safe = static_cast<CompanyOperatorOutput*>(out_safe_sp.get());
  const auto out_safe_json = ParseJsonOutput(out_safe);

  for (const auto* output : {out_violation, out_safe}) {
    EXPECT_EQ(output->service_type, kMockServiceDialogueAudit);
    EXPECT_EQ(output->status_code, 0);
  }

  // 验证样本 A 判定为 HIGH_RISK，且命中对应合规条款
  ASSERT_TRUE(out_violation_json.contains("risk_level"));
  EXPECT_EQ(out_violation_json.at("risk_level").get<std::string>(),
            "HIGH_RISK");
  EXPECT_GE(out_violation_json.at("risk_score").get<float>(), 0.80f);
  ASSERT_TRUE(out_violation_json.contains("matched_policy_clause"));
  std::string matched =
      out_violation_json.at("matched_policy_clause").get<std::string>();
  EXPECT_TRUE(matched.find("退货") != std::string::npos ||
              matched.find("条款") != std::string::npos);

  ASSERT_TRUE(out_violation_json.contains("audit_verdict"));
  const auto& j_violation = out_violation_json.at("audit_verdict");
  EXPECT_EQ(j_violation["risk_level"], "HIGH_RISK");

  // 验证样本 B 判定为 SAFE
  ASSERT_TRUE(out_safe_json.contains("risk_level"));
  EXPECT_EQ(out_safe_json.at("risk_level").get<std::string>(), "SAFE");
  EXPECT_LE(out_safe_json.at("risk_score").get<float>(), 0.40f);

  ASSERT_TRUE(out_safe_json.contains("audit_verdict"));
  EXPECT_EQ(out_safe_json.at("audit_verdict").at("risk_level"), "SAFE");
  out_viol_sp.reset();
  out_safe_sp.reset();
  outputs.clear();

  EXPECT_EQ(instance.Close(), 0) << instance.close_diagnostic();
}

}  // namespace llm_edgeflow

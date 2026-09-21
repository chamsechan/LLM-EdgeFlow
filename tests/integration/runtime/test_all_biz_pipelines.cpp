#include <gtest/gtest.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "edgeflow/operator/interface.h"
#include "edgeflow/operator/types.h"
#include "platform_mock/operator_data_types.h"

namespace llm_edgeflow {

class AllBizPipelinesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    operator_api::Get_LLM_EDGEFLOW_OperatorTable().Init();
  }
  void TearDown() override {
    operator_api::Get_LLM_EDGEFLOW_OperatorTable().DeInit();
  }
};

// 1. 业务 3 (智能长文档切片问答 RAG) 细粒度断言测试 (DocChunk -> Embedding ->
// VectorSearch -> Prompt -> LLM)
TEST_F(AllBizPipelinesTest, DocQaPipelineExecution) {
  operator_api::CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "demo/fixtures/mock/pipeline_doc_qa.conf";
  param.device_id = 0;
  param.compute_platform = operator_api::ComputePlatform::kCpu;
  param.max_frame_depth = 25;

  auto op = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  void* handle = nullptr;
  int ret = op.Create(&handle, &param);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(handle, nullptr);

  std::string doc_text =
      "第一章 "
      "平台注册规范：用户须使用真实身份信息注册，禁止恶意注册多个账号。\n"
      "第二章 "
      "售后退款条例：平台支持自签收之日起7天无理由退货，商品需保持完好不影响二"
      "次销售，审核通过后即时原路返还款项。\n"
      "第三章 "
      "商家发货时效：普通商品应在48小时内完成发货并录入物流单号，定制商品以约定"
      "为准。\n"
      "第四章 "
      "跨境手续费说明：使用境外信用卡进行交易时，结算通道将收取3%"
      "跨境支付综合服务费。\n"
      "第五章 "
      "违禁品管控规则：严禁在平台发布、宣传、交易任何国家法律法规禁止流通的违禁"
      "商品。";

  std::string q0 = "请问平台支持7天无理由退款吗？具体要求是什么？";
  std::string q1 = "跨境信用卡支付要收手续费吗？";

  CompanyString cs_doc{static_cast<int32_t>(doc_text.size()),
                       const_cast<char*>(doc_text.data())};
  CompanyString cs_q0{static_cast<int32_t>(q0.size()),
                      const_cast<char*>(q0.data())};
  CompanyString cs_q1{static_cast<int32_t>(q1.size()),
                      const_cast<char*>(q1.data())};

  CompanyOperatorDocInput req0{30001, &cs_doc, &cs_q0};
  CompanyOperatorDocInput req1{30002, &cs_doc, &cs_q1};

  operator_api::NamedIoBatch inputs(2);
  inputs[0]["rag_channel.doc_in"] =
      operator_api::MakeBorrowedOperatorInput(&req0);
  inputs[1]["rag_channel.doc_in"] =
      operator_api::MakeBorrowedOperatorInput(&req1);

  operator_api::NamedIoBatch outputs(2);
  outputs[0]["rag_channel.doc_out"] = nullptr;
  outputs[1]["rag_channel.doc_out"] = nullptr;

  ret = op.Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(outputs.size(), 2u);

  auto out0_sp = outputs[0]["rag_channel.doc_out"];
  auto out1_sp = outputs[1]["rag_channel.doc_out"];
  ASSERT_NE(out0_sp, nullptr);
  ASSERT_NE(out1_sp, nullptr);

  auto* out0 = static_cast<CompanyOperatorDocOutput*>(out0_sp.get());
  auto* out1 = static_cast<CompanyOperatorDocOutput*>(out1_sp.get());

  // 验证切片数与意图分类
  EXPECT_EQ(out0->request_id, 30001ULL);
  EXPECT_GT(out0->chunk_count, 0);
  ASSERT_NE(out0->intent_name, nullptr);
  EXPECT_EQ(std::string(out0->intent_name->data, out0->intent_name->length),
            "AFTER_SALES_REFUND");
  ASSERT_NE(out0->answer_text, nullptr);
  EXPECT_GT(out0->answer_text->length, 0);

  EXPECT_EQ(out1->request_id, 30002ULL);
  EXPECT_GT(out1->chunk_count, 0);
  ASSERT_NE(out1->answer_text, nullptr);
  out0_sp.reset();
  out1_sp.reset();
  outputs.clear();

  ret = op.Destroy(handle);
  EXPECT_EQ(ret, 0);
}

// 2. 业务 4 (智能对话风控质检 - 3模型6节点级联) 细粒度高危与合规样本双向校验
TEST_F(AllBizPipelinesTest, DialogueComplianceAuditPipeline) {
  operator_api::CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "demo/fixtures/mock/pipeline_dialogue_audit.conf";
  param.device_id = 0;
  param.compute_platform = operator_api::ComputePlatform::kCpu;
  param.max_frame_depth = 25;

  auto op = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  void* handle = nullptr;
  int ret = op.Create(&handle, &param);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(handle, nullptr);

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

  CompanyOperatorAuditInput req_violation{40001, &cs_viol, &cc_viol};
  CompanyOperatorAuditInput req_safe{40002, &cs_safe, &cc_safe};

  operator_api::NamedIoBatch inputs(2);
  inputs[0]["audit_channel.audit_in"] =
      operator_api::MakeBorrowedOperatorInput(&req_violation);
  inputs[1]["audit_channel.audit_in"] =
      operator_api::MakeBorrowedOperatorInput(&req_safe);

  operator_api::NamedIoBatch outputs(2);
  outputs[0]["audit_channel.audit_out"] = nullptr;
  outputs[1]["audit_channel.audit_out"] = nullptr;

  ret = op.Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(outputs.size(), 2u);

  auto out_viol_sp = outputs[0]["audit_channel.audit_out"];
  auto out_safe_sp = outputs[1]["audit_channel.audit_out"];
  ASSERT_NE(out_viol_sp, nullptr);
  ASSERT_NE(out_safe_sp, nullptr);

  auto* out_violation =
      static_cast<CompanyOperatorAuditOutput*>(out_viol_sp.get());
  auto* out_safe = static_cast<CompanyOperatorAuditOutput*>(out_safe_sp.get());

  // 验证样本 A 判定为 HIGH_RISK，且命中对应合规条款
  EXPECT_EQ(out_violation->request_id, 40001ULL);
  ASSERT_NE(out_violation->risk_level, nullptr);
  EXPECT_EQ(std::string(out_violation->risk_level->data,
                        out_violation->risk_level->length),
            "HIGH_RISK");
  EXPECT_GE(out_violation->risk_score, 0.80f);
  ASSERT_NE(out_violation->matched_policy_clause, nullptr);
  std::string matched(out_violation->matched_policy_clause->data,
                      out_violation->matched_policy_clause->length);
  EXPECT_TRUE(matched.find("退货") != std::string::npos ||
              matched.find("条款") != std::string::npos);

  ASSERT_NE(out_violation->audit_verdict_json, nullptr);
  std::string json_viol(out_violation->audit_verdict_json->data,
                        out_violation->audit_verdict_json->length);
  auto j_violation = nlohmann::json::parse(json_viol);
  EXPECT_EQ(j_violation["risk_level"], "HIGH_RISK");

  // 验证样本 B 判定为 SAFE
  EXPECT_EQ(out_safe->request_id, 40002ULL);
  ASSERT_NE(out_safe->risk_level, nullptr);
  EXPECT_EQ(
      std::string(out_safe->risk_level->data, out_safe->risk_level->length),
      "SAFE");
  EXPECT_LE(out_safe->risk_score, 0.40f);

  ASSERT_NE(out_safe->audit_verdict_json, nullptr);
  std::string json_safe(out_safe->audit_verdict_json->data,
                        out_safe->audit_verdict_json->length);
  auto j_safe = nlohmann::json::parse(json_safe);
  out_viol_sp.reset();
  out_safe_sp.reset();
  outputs.clear();

  ret = op.Destroy(handle);
  EXPECT_EQ(ret, 0);
}

}  // namespace llm_edgeflow

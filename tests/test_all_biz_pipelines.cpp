#include <gtest/gtest.h>

#include <cstring>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "company_alg_cpp.hpp"
#include "company_alg_interface.h"

std::string GetConfigPath(const std::string& rel_path) {
  FILE* fp = fopen(rel_path.c_str(), "r");
  if (fp) {
    fclose(fp);
    return rel_path;
  }
  return "../" + rel_path;
}

namespace alg_framework {

class AllBizPipelinesTest : public ::testing::Test {
 protected:
  void SetUp() override { Alg_Init(); }
  void TearDown() override { Alg_DeInit(); }
};

// 1. 业务 3 (智能长文档切片问答 RAG) 细粒度断言测试 (DocChunk -> Embedding ->
// VectorSearch -> Prompt -> LLM)
TEST_F(AllBizPipelinesTest, DocQaPipelineExecution) {
  std::string cfg_path = GetConfigPath("configs/pipeline_doc_qa.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg_path.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_DOC_QA;

  void* handle = nullptr;
  int ret = Alg_Create(&handle, &param);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(handle, nullptr);

  const char* doc_text =
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

  CompanyDocInputStruct req0{30001, doc_text,
                             "请问平台支持7天无理由退款吗？具体要求是什么？"};
  CompanyDocInputStruct req1{30002, doc_text, "跨境信用卡支付要收手续费吗？"};
  std::vector<void*> inputs = {&req0, &req1};

  CompanyDocOutputStruct out0;
  CompanyDocOutputStruct out1;
  std::vector<void*> outputs = {&out0, &out1};

  ret = Alg_Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);

  // 验证切片数与意图分类
  EXPECT_EQ(out0.request_id, 30001);
  EXPECT_GT(out0.chunk_count, 0);
  EXPECT_STREQ(out0.intent_name, "AFTER_SALES_REFUND");
  EXPECT_TRUE(strlen(out0.answer_text) > 0);

  EXPECT_EQ(out1.request_id, 30002);
  EXPECT_GT(out1.chunk_count, 0);
  EXPECT_TRUE(strlen(out1.answer_text) > 0);

  ret = Alg_Destroy(handle);
  EXPECT_EQ(ret, 0);
}

// 2. 业务 4 (智能对话风控质检 - 3模型6节点级联) 细粒度高危与合规样本双向校验
TEST_F(AllBizPipelinesTest, DialogueComplianceAuditPipeline) {
  std::string cfg_path = GetConfigPath("configs/pipeline_dialogue_audit.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg_path.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_COMPLIANCE_AUDIT;

  void* handle = nullptr;
  int ret = Alg_Create(&handle, &param);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(handle, nullptr);

  // 样本 A: 违规诱导私下交易
  CompanyAuditInputStruct req_violation{
      40001,
      "亲，平台退款审核太慢了，你加我私人微信转账给我吧，我私下把商品寄给你，还"
      "能返现20元！",
      "VIP专席客服"};

  // 样本 B: 合规正常客服沟通
  CompanyAuditInputStruct req_safe{40002,
                                   "您好，您的商品符合7天无理由退货政策，已为您"
                                   "在系统提交退款换货流程，请保持手机畅通。",
                                   "在线售后IM"};

  std::vector<void*> inputs = {&req_violation, &req_safe};

  CompanyAuditOutputStruct out_violation;
  CompanyAuditOutputStruct out_safe;
  std::vector<void*> outputs = {&out_violation, &out_safe};

  ret = Alg_Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);

  // 验证样本 A 判定为 HIGH_RISK，且命中对应合规条款
  EXPECT_EQ(out_violation.request_id, 40001);
  EXPECT_STREQ(out_violation.risk_level, "HIGH_RISK");
  EXPECT_GE(out_violation.risk_score, 0.80f);
  EXPECT_TRUE(std::string(out_violation.matched_policy_clause).find("退货") !=
                  std::string::npos ||
              std::string(out_violation.matched_policy_clause).find("条款") !=
                  std::string::npos);

  auto j_violation = nlohmann::json::parse(out_violation.audit_verdict_json);
  EXPECT_EQ(j_violation["risk_level"], "HIGH_RISK");

  // 验证样本 B 判定为 SAFE
  EXPECT_EQ(out_safe.request_id, 40002);
  EXPECT_STREQ(out_safe.risk_level, "SAFE");
  EXPECT_LE(out_safe.risk_score, 0.40f);

  auto j_safe = nlohmann::json::parse(out_safe.audit_verdict_json);
  EXPECT_EQ(j_safe["risk_level"], "SAFE");

  ret = Alg_Destroy(handle);
  EXPECT_EQ(ret, 0);
}

}  // namespace alg_framework

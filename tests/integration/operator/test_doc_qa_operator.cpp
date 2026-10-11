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

using DocQaOperatorTest = test_support::OperatorTestFixture;

TEST_F(DocQaOperatorTest, ArchitectureRequest) {
  using namespace llm_edgeflow::operator_api;
  ScopedTestOperator instance(ops_);
  ASSERT_EQ(instance.Create("demo/fixtures/mock/pipeline_doc_qa.conf"), 0)
      << instance.create_diagnostic();
  ASSERT_NE(instance.get(), nullptr);

  std::string doc =
      "LLM-EdgeFlow 采用 C++ 实现高性能端侧推理架构与 Pipeline 调度设计。";
  std::string query = "LLM-EdgeFlow 的架构设计是什么？";
  CompanyString cs_doc{static_cast<int32_t>(doc.size()),
                       const_cast<char*>(doc.data())};
  CompanyString cs_query{static_cast<int32_t>(query.size()),
                         const_cast<char*>(query.data())};

  CompanyOperatorDocInput in{};
  in.service_type = kMockServiceDocQa;
  in.doc_text = &cs_doc;
  in.query_text = &cs_query;

  NamedIoBatch inputs(1);
  inputs[0]["rag_channel.doc_in"] = MakeBorrowedOperatorInput(&in);

  NamedIoBatch outputs(1);
  outputs[0]["rag_channel.json_out"] = std::shared_ptr<void>();

  int p_ret = ops_.Process(instance.get(), inputs, outputs);
  ASSERT_EQ(p_ret, 0) << "Process error: "
                      << llm_edgeflow::operator_api::GetOperatorLastError();
  auto out_sp = outputs[0]["rag_channel.json_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_dto = static_cast<CompanyOperatorOutput*>(out_sp.get());
  const auto out_dto_json = ParseJsonOutput(out_dto);
  EXPECT_EQ(out_dto->service_type, kMockServiceDocQa);
  EXPECT_EQ(out_dto->status_code, 0);
  ASSERT_TRUE(out_dto_json.contains("intent_name"));
  EXPECT_STREQ(out_dto_json.at("intent_name").get<std::string>().c_str(),
               "TECH_ARCHITECTURE");
  EXPECT_GT(out_dto_json.at("confidence").get<float>(), 0.5f);
  EXPECT_GT(out_dto_json.at("chunk_count").get<int>(), 0);
  ASSERT_TRUE(out_dto_json.contains("answer_text"));
  EXPECT_GT(out_dto_json.at("answer_text").get<std::string>().size(), 0);

  outputs.clear();
  out_sp.reset();
  EXPECT_EQ(instance.Close(), 0) << instance.close_diagnostic();
}

TEST_F(DocQaOperatorTest, MultipleQuestions) {
  ScopedTestOperator instance(ops_);
  ASSERT_EQ(instance.Create("demo/fixtures/mock/pipeline_doc_qa.conf"), 0)
      << instance.create_diagnostic();
  ASSERT_NE(instance.get(), nullptr);

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

  CompanyOperatorDocInput req0{kMockServiceDocQa, &cs_doc, &cs_q0};
  std::string second_doc = "境外信用卡交易收取3%跨境支付综合服务费。";
  CompanyString cs_second_doc{static_cast<int32_t>(second_doc.size()),
                              second_doc.data()};
  CompanyOperatorDocInput req1{kMockServiceDocQa, &cs_second_doc, &cs_q1};

  operator_api::NamedIoBatch inputs(2);
  inputs[0]["rag_channel.doc_in"] =
      operator_api::MakeBorrowedOperatorInput(&req0);
  inputs[1]["rag_channel.doc_in"] =
      operator_api::MakeBorrowedOperatorInput(&req1);

  operator_api::NamedIoBatch outputs(2);
  outputs[0]["rag_channel.json_out"] = nullptr;
  outputs[1]["rag_channel.json_out"] = nullptr;

  const int ret = ops_.Process(instance.get(), inputs, outputs);
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(outputs.size(), 2u);

  auto out0_sp = outputs[0]["rag_channel.json_out"];
  auto out1_sp = outputs[1]["rag_channel.json_out"];
  ASSERT_NE(out0_sp, nullptr);
  ASSERT_NE(out1_sp, nullptr);

  auto* out0 = static_cast<CompanyOperatorOutput*>(out0_sp.get());
  const auto out0_json = ParseJsonOutput(out0);
  auto* out1 = static_cast<CompanyOperatorOutput*>(out1_sp.get());
  const auto out1_json = ParseJsonOutput(out1);

  // 验证切片数与意图分类
  EXPECT_GT(out0_json.at("chunk_count").get<int>(), 0);
  ASSERT_TRUE(out0_json.contains("intent_name"));
  EXPECT_EQ(out0_json.at("intent_name").get<std::string>(),
            "AFTER_SALES_REFUND");
  ASSERT_TRUE(out0_json.contains("answer_text"));
  EXPECT_GT(out0_json.at("answer_text").get<std::string>().size(), 0);

  EXPECT_GT(out1_json.at("chunk_count").get<int>(), 0);
  ASSERT_TRUE(out1_json.contains("answer_text"));
  ASSERT_TRUE(out1_json.contains("intent_name"));
  EXPECT_GT(out1_json.at("answer_text").get<std::string>().size(), 0);
  out0_sp.reset();
  out1_sp.reset();
  outputs.clear();

  EXPECT_EQ(instance.Close(), 0) << instance.close_diagnostic();
}

}  // namespace llm_edgeflow

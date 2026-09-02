#include <gtest/gtest.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "company_alg_interface.h"
#include "operator/company_operator_types.h"
#include "operator/operator_interface.h"

using namespace llm_edgeflow::operator_api;

static std::string GetConfDir() {
  if (std::filesystem::exists("configs")) {
    return std::filesystem::current_path().string();
  }
  return std::filesystem::current_path().parent_path().string();
}

class DocQaRerankPipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ops_ = Get_LLM_EDGEFLOW_OperatorTable();
    ASSERT_EQ(ops_.Init(), 0);
  }

  void TearDown() override { ASSERT_EQ(ops_.Deinit(), 0); }

  OperatorFunc ops_;
};

// 1. 测试基于 Operator 创建与执行 LLM + Rerank + QA 组合流水线
TEST_F(DocQaRerankPipelineTest, ExecuteDocQaWithRerankerAndLlm) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "demo/fixtures/mock/pipeline_doc_qa_rerank.conf";
  param.compute_platform = ComputePlatform::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  int ret = ops_.Create(&handle, &param);
  ASSERT_EQ(ret, 0) << "Create failed: " << GetOperatorLastError();
  ASSERT_NE(handle, nullptr);

  std::string doc1 =
      "企业级算法框架设计规范：采用4层分层架构，包含C-"
      "ABI适配层、Pipeline调度层、通用算子池与底层硬件引擎抽象。"
      "其中RerankRefineNode算子用于在粗筛后进行高精度的Cross-"
      "Encoder语义重排打分。";
  std::string query1 = "请问该框架中的Rerank算子有什么作用？";
  CompanyString doc1_cs{static_cast<int32_t>(doc1.size()),
                        const_cast<char*>(doc1.data())};
  CompanyString query1_cs{static_cast<int32_t>(query1.size()),
                          const_cast<char*>(query1.data())};
  CompanyOperatorDocInput in1{90001, &doc1_cs, &query1_cs};

  std::string doc2 =
      "客户服务售后政策：支持7天无理由退货与全额退款。若商品存在质量问题，由平"
      "台承担双向运费并提供快速换货。";
  std::string query2 = "商品质量有问题怎么换货？";
  CompanyString doc2_cs{static_cast<int32_t>(doc2.size()),
                        const_cast<char*>(doc2.data())};
  CompanyString query2_cs{static_cast<int32_t>(query2.size()),
                          const_cast<char*>(query2.data())};
  CompanyOperatorDocInput in2{90002, &doc2_cs, &query2_cs};

  NamedIoBatch in_batch(2);
  NamedIoBatch out_batch(2);

  in_batch[0]["rag_channel.doc_in"] = MakeBorrowedOperatorInput(&in1);
  in_batch[1]["rag_channel.doc_in"] = MakeBorrowedOperatorInput(&in2);

  out_batch[0]["rag_channel.doc_out"] = std::shared_ptr<void>();
  out_batch[1]["rag_channel.doc_out"] = std::shared_ptr<void>();

  ret = ops_.Process(handle, in_batch, out_batch);
  EXPECT_EQ(ret, 0) << "Process failed: " << GetOperatorLastError();

  auto out1_sp = out_batch[0]["rag_channel.doc_out"];
  auto out2_sp = out_batch[1]["rag_channel.doc_out"];
  ASSERT_NE(out1_sp, nullptr);
  ASSERT_NE(out2_sp, nullptr);

  const auto* out1 =
      static_cast<const CompanyOperatorDocOutput*>(out1_sp.get());
  const auto* out2 =
      static_cast<const CompanyOperatorDocOutput*>(out2_sp.get());

  EXPECT_EQ(out1->request_id, 90001u);
  EXPECT_GT(out1->chunk_count, 0);
  ASSERT_NE(out1->intent_name, nullptr);
  EXPECT_STREQ(out1->intent_name->data, "TECH_ARCHITECTURE");
  ASSERT_NE(out1->answer_text, nullptr);
  EXPECT_GT(out1->answer_text->length, 0);

  EXPECT_EQ(out2->request_id, 90002u);
  EXPECT_GT(out2->chunk_count, 0);
  ASSERT_NE(out2->intent_name, nullptr);
  EXPECT_STREQ(out2->intent_name->data, "AFTER_SALES_REFUND");
  ASSERT_NE(out2->answer_text, nullptr);
  EXPECT_GT(out2->answer_text->length, 0);

  out_batch.clear();
  out1_sp.reset();
  out2_sp.reset();

  ops_.Destroy(handle);
}

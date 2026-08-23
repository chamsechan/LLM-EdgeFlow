#include <gtest/gtest.h>

#include <iostream>
#include <string>
#include <vector>

#include "company_alg_interface.h"
#include "platform/platform_operator_interface.h"

using namespace llm_edgeflow::platform;

static std::string GetConfPath(const std::string& rel_path) {
  FILE* fp = fopen(rel_path.c_str(), "r");
  if (fp) {
    fclose(fp);
    return rel_path;
  }
  return "../" + rel_path;
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

// 1. 测试基于 Platform Operator 创建与执行 LLM + Rerank + QA 组合流水线
TEST_F(DocQaRerankPipelineTest, ExecuteDocQaWithRerankerAndLlm) {
  std::string conf_path = GetConfPath("configs/pipeline_doc_qa_rerank.conf");
  CreateParam param{};
  param.cfg_file_name = conf_path.c_str();
  param.platform_config.batch_size = 2;
  param.platform_config.type = ChipType::kAx650;
  param.depth_num = 2;

  void* handle = nullptr;
  int ret = ops_.Create(&handle, &param);
  ASSERT_EQ(ret, 0) << "Create failed: " << GetPlatformLastError();
  ASSERT_NE(handle, nullptr);

  // 构造输入样本
  CompanyDocInputStruct in1{};
  in1.request_id = 90001;
  in1.doc_text =
      "企业级算法框架设计规范：采用4层分层架构，包含C-"
      "ABI适配层、Pipeline调度层、通用算子池与底层硬件引擎抽象。"
      "其中RerankRefineNode算子用于在粗筛后进行高精度的Cross-"
      "Encoder语义重排打分。";
  in1.query_text = "请问该框架中的Rerank算子有什么作用？";

  CompanyDocInputStruct in2{};
  in2.request_id = 90002;
  in2.doc_text =
      "客户服务售后政策：支持7天无理由退货与全额退款。若商品存在质量问题，由平"
      "台承担双向运费并提供快速换货。";
  in2.query_text = "商品质量有问题怎么换货？";

  CompanyDocOutputStruct out1{};
  CompanyDocOutputStruct out2{};

  NamedIoBatch in_batch(2);
  NamedIoBatch out_batch(2);

  in_batch[0]["rag_channel.doc_in"] = std::shared_ptr<void>(&in1, [](void*) {});
  in_batch[1]["rag_channel.doc_in"] = std::shared_ptr<void>(&in2, [](void*) {});

  out_batch[0]["rag_channel.doc_out"] =
      std::shared_ptr<void>(&out1, [](void*) {});
  out_batch[1]["rag_channel.doc_out"] =
      std::shared_ptr<void>(&out2, [](void*) {});

  ret = ops_.Process(handle, in_batch, out_batch);
  EXPECT_EQ(ret, 0) << "Process failed: " << GetPlatformLastError();

  // 验证结果
  EXPECT_EQ(out1.request_id, 90001);
  EXPECT_GT(out1.chunk_count, 0);
  EXPECT_STREQ(out1.intent_name, "TECH_ARCHITECTURE");
  EXPECT_GT(strlen(out1.answer_text), 0);

  EXPECT_EQ(out2.request_id, 90002);
  EXPECT_GT(out2.chunk_count, 0);
  EXPECT_STREQ(out2.intent_name, "AFTER_SALES_REFUND");
  EXPECT_GT(strlen(out2.answer_text), 0);

  std::cout << "[DocQaRerankPipelineTest] Output 1 intent=" << out1.intent_name
            << ", answer=" << out1.answer_text << std::endl;
  std::cout << "[DocQaRerankPipelineTest] Output 2 intent=" << out2.intent_name
            << ", answer=" << out2.answer_text << std::endl;

  ops_.Destroy(handle);
}

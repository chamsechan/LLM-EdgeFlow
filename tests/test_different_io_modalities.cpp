#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "company_alg_cpp.hpp"
#include "company_alg_interface.h"
#include "engine/backend_registry.h"

static std::string GetConfigPath(const std::string& rel_path) {
  FILE* fp = fopen(rel_path.c_str(), "r");
  if (fp) {
    fclose(fp);
    return rel_path;
  }
  return "../" + rel_path;
}

namespace alg_framework {

class DifferentIoModalitiesTest : public ::testing::Test {
 protected:
  void SetUp() override { Alg_Init(); }
  void TearDown() override { Alg_DeInit(); }
};

// 1. 验证业务 5: 多模态图文票据问答 (Image + Query -> OCR BBox -> LLM JSON)
TEST_F(DifferentIoModalitiesTest, OcrDocQa) {
  std::string cfg_path =
      GetConfigPath("tests/fixtures/stage7/smoke/pipeline_ocr_doc_qa.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg_path.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_OCR_DOC_QA;

  void* handle = nullptr;
  int ret = Alg_Create(&handle, &param);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(handle, nullptr);

  CompanyOcrDocInputStruct in_req1{60001, "./data/invoice_sample_01.jpg",
                                   "提取发票代码、号码与总金额"};
  CompanyOcrDocInputStruct in_req2{60002, "./data/vat_receipt_02.png",
                                   "提取购买方公司名称与税额"};
  std::vector<void*> inputs = {&in_req1, &in_req2};

  CompanyOcrDocOutputStruct out1;
  CompanyOcrDocOutputStruct out2;
  std::vector<void*> outputs = {&out1, &out2};

  ret = Alg_Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out1.request_id, 60001ULL);
  EXPECT_EQ(out1.detected_box_count, 6U);
  EXPECT_EQ(out2.request_id, 60002ULL);
  EXPECT_EQ(out2.detected_box_count, 6U);

  auto j1 = nlohmann::json::parse(out1.extracted_invoice_json);
  EXPECT_TRUE(j1.contains("invoice_code") && j1.contains("total_amount"));

  ret = Alg_Destroy(handle);
  EXPECT_EQ(ret, 0);
}

// 2. 验证业务 6: 语音识别与时序意图槽位抽取 (Float PCM Buffer -> Speech Text ->
// NLU Intent/Slots)
TEST_F(DifferentIoModalitiesTest, AudioAsrIntent) {
  std::string cfg_path = GetConfigPath(
      "tests/fixtures/stage7/smoke/pipeline_audio_asr_intent.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg_path.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_AUDIO_ASR_INTENT;

  void* handle = nullptr;
  int ret = Alg_Create(&handle, &param);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(handle, nullptr);

  // 构造两个模拟的音频 PCM 浮点信号
  std::vector<float> pcm1(16000, 0.01f);   // 导航语音 (累计值较大)
  std::vector<float> pcm2(16000, 0.001f);  // 空调车控语音

  CompanyAudioInputStruct in_audio1{70001, pcm1.data(),
                                    static_cast<int>(pcm1.size()), 16000};
  CompanyAudioInputStruct in_audio2{70002, pcm2.data(),
                                    static_cast<int>(pcm2.size()), 16000};
  std::vector<void*> inputs = {&in_audio1, &in_audio2};

  CompanyAudioOutputStruct out1;
  CompanyAudioOutputStruct out2;
  std::vector<void*> outputs = {&out1, &out2};

  ret = Alg_Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out1.request_id, 70001);
  EXPECT_EQ(out2.request_id, 70002);

  auto j1 = nlohmann::json::parse(out1.intent_slot_json);
  auto j2 = nlohmann::json::parse(out2.intent_slot_json);
  EXPECT_EQ(j1["intent"], "NAVIGATION");
  EXPECT_EQ(j2["intent"], "VEHICLE_HVAC_CONTROL");

  ret = Alg_Destroy(handle);
  EXPECT_EQ(ret, 0);
}

// 3. 验证业务 7: 纯语义精排矩阵打分 (1 Query + N Candidate Passages -> Matrix
// Scores -> Top-K Indices)
TEST_F(DifferentIoModalitiesTest, CrossRerankBatch) {
#ifndef HAVE_ONNXRUNTIME
  GTEST_SKIP() << "ONNX Runtime disabled in this build";
#else
  if (!alg_framework::BackendRegistry::Instance()
           .Find("onnxruntime")
           .has_value()) {
    GTEST_SKIP() << "ONNX Runtime backend disabled in this build";
  }

  std::string cfg_path = GetConfigPath("configs/pipeline_cross_rerank.json");
  std::ifstream json_in(cfg_path);
  ASSERT_TRUE(json_in.good());
  nlohmann::json pipe_json;
  json_in >> pipe_json;
  pipe_json["models"][0]["model_path"] = EDGEFLOW_STAGE4_RERANK_ONNX_FIXTURE;
  pipe_json["models"][0]["model_config"]["tokenizer_file"] =
      EDGEFLOW_STAGE3_VOCAB_FIXTURE;
  pipe_json["models"][0]["model_config"]["max_length"] = 32;

  auto temp_dir = std::filesystem::temp_directory_path() /
                  ("test_different_io_rerank_" + std::to_string(rand()));
  std::filesystem::create_directories(temp_dir);
  auto temp_cfg_path = temp_dir / "pipeline_cross_rerank.json";
  std::ofstream json_out(temp_cfg_path);
  json_out << pipe_json.dump(2);
  json_out.close();

  std::string temp_cfg_str = temp_cfg_path.string();
  CompanyAlgParamCreate param;
  param.config_file_path = temp_cfg_str.c_str();
  param.model_root_dir = "";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_CROSS_RERANK;

  void* handle = nullptr;
  int ret = Alg_Create(&handle, &param);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(handle, nullptr);

  const char* candidates[5] = {
      "条款A: 仅在工作日提供人工客服支持。",
      "条款B: 支持7天无理由退货政策，审核通过后即时原路返还资金。",
      "条款C: 境外信用卡交易收取3%跨境手续费。",
      "条款D: 电子发票在订单完成后24小时内发送至邮箱。",
      "条款E: VIP用户享受专属1对1客服通道与快速理赔。"};

  CompanyRerankBatchInputStruct in_rerank;
  in_rerank.request_id = 80001;
  in_rerank.query_text = "请问如何申请7天无理由退款？";
  in_rerank.candidate_count = 5;
  for (int i = 0; i < 5; ++i) in_rerank.candidate_passages[i] = candidates[i];

  std::vector<void*> inputs = {&in_rerank};
  CompanyRerankBatchOutputStruct out_rerank;
  std::vector<void*> outputs = {&out_rerank};

  ret = Alg_Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out_rerank.request_id, 80001);
  EXPECT_EQ(out_rerank.count, 5);

  for (int i = 0; i < out_rerank.count; ++i) {
    if (i > 0) {
      EXPECT_GE(out_rerank.scores[i - 1], out_rerank.scores[i]);
    }
  }

  ret = Alg_Destroy(handle);
  EXPECT_EQ(ret, 0);
  std::error_code ec;
  std::filesystem::remove_all(temp_dir, ec);
#endif
}

}  // namespace alg_framework

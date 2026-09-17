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

#include "edgeflow/operator/interface.h"
#include "edgeflow/operator/types.h"
#include "engine/backend_registry.h"
#include "platform_mock/operator_data_types.h"

static std::string GetConfigPath(const std::string& rel_path) {
  FILE* fp = fopen(rel_path.c_str(), "r");
  if (fp) {
    fclose(fp);
    return rel_path;
  }
  return "../" + rel_path;
}

namespace llm_edgeflow {

class DifferentIoModalitiesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    operator_api::Get_LLM_EDGEFLOW_OperatorTable().Init();
  }
  void TearDown() override {
    operator_api::Get_LLM_EDGEFLOW_OperatorTable().Deinit();
  }
};

// 1. 验证业务 5: 多模态图文票据问答 (Image + Query -> OCR BBox -> LLM JSON)
TEST_F(DifferentIoModalitiesTest, OcrDocQa) {
  auto op = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  operator_api::CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "demo/fixtures/mock/pipeline_ocr_doc_qa.conf";
  param.device_id = 0;
  param.compute_platform = operator_api::ComputePlatform::kCpu;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  int ret = op.Create(&handle, &param);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(handle, nullptr);

  std::string img1 = "./data/invoice_sample_01.jpg";
  std::string p1 = "提取发票代码、号码与总金额";
  std::string img2 = "./data/vat_receipt_02.png";
  std::string p2 = "提取购买方公司名称与税额";

  CompanyString img1_cs{static_cast<int32_t>(img1.size()),
                        const_cast<char*>(img1.data())};
  CompanyFrame frame1{60001, &img1_cs, nullptr};
  CompanyString p1_cs{static_cast<int32_t>(p1.size()),
                      const_cast<char*>(p1.data())};

  CompanyString img2_cs{static_cast<int32_t>(img2.size()),
                        const_cast<char*>(img2.data())};
  CompanyFrame frame2{60002, &img2_cs, nullptr};
  CompanyString p2_cs{static_cast<int32_t>(p2.size()),
                      const_cast<char*>(p2.data())};

  operator_api::NamedIoBatch inputs(2);
  inputs[0]["camera_0.frame"] =
      operator_api::MakeBorrowedOperatorInput(&frame1);
  inputs[0]["camera_0.string"] =
      operator_api::MakeBorrowedOperatorInput(&p1_cs);
  inputs[1]["camera_0.frame"] =
      operator_api::MakeBorrowedOperatorInput(&frame2);
  inputs[1]["camera_0.string"] =
      operator_api::MakeBorrowedOperatorInput(&p2_cs);

  operator_api::NamedIoBatch outputs(2);
  outputs[0]["camera_0.od_out"] = nullptr;
  outputs[1]["camera_0.od_out"] = nullptr;

  ret = op.Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(outputs.size(), 2u);

  auto out1_sp = outputs[0]["camera_0.od_out"];
  auto out2_sp = outputs[1]["camera_0.od_out"];
  ASSERT_NE(out1_sp, nullptr);
  ASSERT_NE(out2_sp, nullptr);

  auto* out1 = static_cast<CompanyOdOutput*>(out1_sp.get());
  auto* out2 = static_cast<CompanyOdOutput*>(out2_sp.get());

  EXPECT_EQ(out1->request_id, 60001ULL);
  EXPECT_EQ(out1->detected_box_count, 6);
  EXPECT_EQ(out2->request_id, 60002ULL);
  EXPECT_EQ(out2->detected_box_count, 6);

  ASSERT_NE(out1->result_json, nullptr);
  std::string json_str1(out1->result_json->data, out1->result_json->length);
  auto j1 = nlohmann::json::parse(json_str1);
  EXPECT_TRUE(j1.contains("invoice_code") && j1.contains("total_amount"));

  out1_sp.reset();
  out2_sp.reset();
  outputs.clear();
  ret = op.Destroy(handle);
  EXPECT_EQ(ret, 0);
}

// 2. 验证业务 6: 语音识别与时序意图槽位抽取 (Float PCM Buffer -> Speech Text ->
// NLU Intent/Slots)
TEST_F(DifferentIoModalitiesTest, AudioAsrIntent) {
  auto op = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  operator_api::CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "demo/fixtures/mock/pipeline_audio_asr_intent.conf";
  param.device_id = 0;
  param.compute_platform = operator_api::ComputePlatform::kCpu;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  int ret = op.Create(&handle, &param);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(handle, nullptr);

  // 构造两个模拟的音频 PCM 浮点信号
  std::vector<float> pcm1(16000, 0.01f);   // 导航语音 (累计值较大)
  std::vector<float> pcm2(16000, 0.001f);  // 空调车控语音

  CompanyOperatorAudioInput in_audio1{70001, pcm1.data(),
                                      static_cast<int32_t>(pcm1.size()), 16000};
  CompanyOperatorAudioInput in_audio2{70002, pcm2.data(),
                                      static_cast<int32_t>(pcm2.size()), 16000};

  operator_api::NamedIoBatch inputs(2);
  inputs[0]["mic_0.audio_in"] =
      operator_api::MakeBorrowedOperatorInput(&in_audio1);
  inputs[1]["mic_0.audio_in"] =
      operator_api::MakeBorrowedOperatorInput(&in_audio2);

  operator_api::NamedIoBatch outputs(2);
  outputs[0]["mic_0.audio_out"] = nullptr;
  outputs[1]["mic_0.audio_out"] = nullptr;

  ret = op.Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(outputs.size(), 2u);

  auto out1_sp = outputs[0]["mic_0.audio_out"];
  auto out2_sp = outputs[1]["mic_0.audio_out"];
  ASSERT_NE(out1_sp, nullptr);
  ASSERT_NE(out2_sp, nullptr);

  auto* out1 = static_cast<CompanyOperatorAudioOutput*>(out1_sp.get());
  auto* out2 = static_cast<CompanyOperatorAudioOutput*>(out2_sp.get());

  EXPECT_EQ(out1->request_id, 70001ULL);
  EXPECT_EQ(out2->request_id, 70002ULL);

  ASSERT_NE(out1->intent_slot_json, nullptr);
  std::string s1(out1->intent_slot_json->data, out1->intent_slot_json->length);
  auto j1 = nlohmann::json::parse(s1);

  ASSERT_NE(out2->intent_slot_json, nullptr);
  std::string s2(out2->intent_slot_json->data, out2->intent_slot_json->length);
  auto j2 = nlohmann::json::parse(s2);

  EXPECT_EQ(j1["intent"], "NAVIGATION");
  EXPECT_EQ(j2["intent"], "VEHICLE_HVAC_CONTROL");

  CompanyOperatorAudioInput empty1{70001, nullptr, 0, 16000};
  CompanyOperatorAudioInput empty2{70002, nullptr, 0, 16000};
  inputs[0]["mic_0.audio_in"] =
      operator_api::MakeBorrowedOperatorInput(&empty1);
  inputs[1]["mic_0.audio_in"] =
      operator_api::MakeBorrowedOperatorInput(&empty2);
  outputs[0]["mic_0.audio_out"] = nullptr;
  outputs[1]["mic_0.audio_out"] = nullptr;

  EXPECT_EQ(op.Process(handle, inputs, outputs), 0);
  out1_sp = outputs[0]["mic_0.audio_out"];
  out2_sp = outputs[1]["mic_0.audio_out"];
  ASSERT_NE(out1_sp, nullptr);
  ASSERT_NE(out2_sp, nullptr);
  out1 = static_cast<CompanyOperatorAudioOutput*>(out1_sp.get());
  out2 = static_cast<CompanyOperatorAudioOutput*>(out2_sp.get());
  EXPECT_EQ(out1->request_id, 70001ULL);
  EXPECT_EQ(out2->request_id, 70002ULL);
  EXPECT_EQ(out1->status_code, 0);
  EXPECT_EQ(out2->status_code, 0);

  out1_sp.reset();
  out2_sp.reset();
  outputs.clear();
  ret = op.Destroy(handle);
  EXPECT_EQ(ret, 0);
}

// 3. 验证业务 7: 纯语义精排矩阵打分 (1 Query + N Candidate Passages -> Matrix
// Scores -> Top-K Indices)
TEST_F(DifferentIoModalitiesTest, CrossRerankBatch) {
#ifndef HAVE_ONNXRUNTIME
  GTEST_SKIP() << "ONNX Runtime disabled in this build";
#else
  if (!llm_edgeflow::BackendRegistry::Instance()
           .Find("onnxruntime")
           .has_value()) {
    GTEST_SKIP() << "ONNX Runtime backend disabled in this build";
  }

  std::string cfg_path =
      GetConfigPath("configs/pipeline_cross_rerank_cpu.json");
  std::ifstream json_in(cfg_path);
  ASSERT_TRUE(json_in.good());
  nlohmann::json pipe_json;
  json_in >> pipe_json;
  auto temp_dir = std::filesystem::temp_directory_path() /
                  ("test_different_io_rerank_" + std::to_string(rand()));
  auto models_dir = temp_dir / "models";
  std::filesystem::create_directories(models_dir);
  std::error_code copy_ec;
  std::filesystem::copy_file(
      EDGEFLOW_RERANK_ONNX_FIXTURE, models_dir / "rerank.onnx",
      std::filesystem::copy_options::overwrite_existing, copy_ec);
  std::filesystem::copy_file(EDGEFLOW_VOCAB_FIXTURE, models_dir / "vocab.txt",
                             std::filesystem::copy_options::overwrite_existing,
                             copy_ec);

  pipe_json["models"][0]["model_path"] = "models/rerank.onnx";
  pipe_json["models"][0]["model_config"]["tokenizer_file"] = "vocab.txt";
  pipe_json["models"][0]["model_config"]["max_length"] = 32;
  pipe_json["deployment"]["model_paths"]["rerank_model_v1"] =
      "models/rerank.onnx";

  auto temp_pipe_path = temp_dir / "pipeline_cross_rerank.json";
  std::ofstream json_out(temp_pipe_path);
  json_out << pipe_json.dump(2);
  json_out.close();

  nlohmann::json deploy_cfg = {{"pipe_path", "pipeline_cross_rerank.json"}};
  auto temp_cfg_path = temp_dir / "pipeline_cross_rerank.conf";
  std::ofstream cfg_out(temp_cfg_path);
  cfg_out << deploy_cfg.dump(2);
  cfg_out.close();

  auto op = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  operator_api::CreateParam param{};
  param.model_path = temp_dir.c_str();
  param.cfg_file_name = "pipeline_cross_rerank.conf";
  param.device_id = 1;
  param.compute_platform = operator_api::ComputePlatform::kCpu;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  int ret = op.Create(&handle, &param);
  EXPECT_NE(ret, 0);
  EXPECT_EQ(handle, nullptr);

  param.device_id = 0;
  ret = op.Create(&handle, &param);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(handle, nullptr);

  std::string query = "请问如何申请7天无理由退款？";
  std::vector<std::string> candidates = {
      "条款A: 仅在工作日提供人工客服支持。",
      "条款B: 支持7天无理由退货政策，审核通过后即时原路返还资金。",
      "条款C: 境外信用卡交易收取3%跨境手续费。",
      "条款D: 电子发票在订单完成后24小时内发送至邮箱。",
      "条款E: VIP用户享受专属1对1客服通道与快速理赔。"};

  CompanyString q_cs{static_cast<int32_t>(query.size()),
                     const_cast<char*>(query.data())};
  std::vector<CompanyString> c_cs;
  c_cs.reserve(5);
  for (int i = 0; i < 5; ++i) {
    c_cs.push_back({static_cast<int32_t>(candidates[i].size()),
                    const_cast<char*>(candidates[i].data())});
  }

  CompanyOperatorRerankInput in_rerank{};
  in_rerank.request_id = 80001;
  in_rerank.query_text = &q_cs;
  in_rerank.candidate_count = 5;
  for (int i = 0; i < 5; ++i) {
    in_rerank.candidate_passages[i] = &c_cs[i];
  }

  operator_api::NamedIoBatch inputs(1);
  inputs[0]["ranker.rerank_in"] =
      operator_api::MakeBorrowedOperatorInput(&in_rerank);
  operator_api::NamedIoBatch outputs(1);
  outputs[0]["ranker.rerank_out"] = nullptr;

  ret = op.Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(outputs.size(), 1u);

  auto out_sp = outputs[0]["ranker.rerank_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_rerank = static_cast<CompanyOperatorRerankOutput*>(out_sp.get());

  EXPECT_EQ(out_rerank->request_id, 80001ULL);
  EXPECT_EQ(out_rerank->count, 5);

  for (int i = 0; i < out_rerank->count; ++i) {
    if (i > 0) {
      EXPECT_GE(out_rerank->scores[i - 1], out_rerank->scores[i]);
    }
  }

  out_sp.reset();
  outputs.clear();
  ret = op.Destroy(handle);
  EXPECT_EQ(ret, 0);
  std::error_code ec;
  std::filesystem::remove_all(temp_dir, ec);
#endif
}

}  // namespace llm_edgeflow

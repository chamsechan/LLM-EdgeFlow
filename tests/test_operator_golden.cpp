#include <gtest/gtest.h>

#include <cstring>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "company_alg_interface.h"
#include "operator/operator_interface.h"

namespace alg_framework {

class OperatorGoldenTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ops_ = llm_edgeflow::operator_api::Get_LLM_EDGEFLOW_OperatorTable();
    ASSERT_EQ(ops_.Init(), 0);
  }

  void TearDown() override { EXPECT_EQ(ops_.Deinit(), 0); }

  llm_edgeflow::operator_api::OperatorFunc ops_;
};

// Golden Test 1: KeywordMatch (Biz 2)
TEST_F(OperatorGoldenTest, KeywordMatchGolden) {
  using namespace llm_edgeflow::operator_api;
  CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_keyword_match.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kCpu;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::string sentence = "客户要求加急处理VIP订单";
  CompanyString cs_sentence{static_cast<int32_t>(sentence.size()),
                            const_cast<char*>(sentence.data())};
  CompanyOperatorKeywordInput in{};
  in.request_id = 1001;
  in.sentence_text = &cs_sentence;

  ControlUpdateRulesParam rules_param{
      "{\"categories\":{\"VIP_SERVICE\":[\"VIP\",\"加急\"]}}"};
  ASSERT_EQ(ops_.Control(handle, ControlCommand::kUpdateRules, &rules_param),
            0);

  NamedIoBatch inputs(1);
  inputs[0]["chan.keyword_in"] = MakeBorrowedOperatorInput(&in);

  NamedIoBatch outputs(1);
  outputs[0]["chan.keyword_out"] = std::shared_ptr<void>();

  int p_ret = ops_.Process(handle, inputs, outputs);
  ASSERT_EQ(p_ret, 0) << "Process error: "
                      << llm_edgeflow::operator_api::GetOperatorLastError();
  auto out_sp = outputs[0]["chan.keyword_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_dto = static_cast<CompanyOperatorKeywordOutput*>(out_sp.get());
  EXPECT_EQ(out_dto->request_id, 1001u);
  EXPECT_EQ(out_dto->is_hit, 1);
  ASSERT_NE(out_dto->match_result_json, nullptr);
  EXPECT_NE(std::string(out_dto->match_result_json->data,
                        out_dto->match_result_json->length)
                .find("VIP"),
            std::string::npos);

  outputs.clear();
  out_sp.reset();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// Golden Test 6: AudioAsrIntent (Biz 6) with Slot Extraction Golden
// Verification
TEST_F(OperatorGoldenTest, AudioAsrIntentSlotExtractionGolden) {
  using namespace llm_edgeflow::operator_api;
  CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_audio_asr_intent.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kCpu;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::vector<float> pcm(16000, 0.05f);
  CompanyOperatorAudioInput in{6001, pcm.data(),
                               static_cast<int32_t>(pcm.size()), 16000};

  NamedIoBatch inputs(1);
  inputs[0]["mic_0.audio_in"] = MakeBorrowedOperatorInput(&in);

  NamedIoBatch outputs(1);
  outputs[0]["mic_0.audio_out"] = std::shared_ptr<void>();

  int p_ret = ops_.Process(handle, inputs, outputs);
  ASSERT_EQ(p_ret, 0) << "Process error: "
                      << llm_edgeflow::operator_api::GetOperatorLastError();
  auto out_sp = outputs[0]["mic_0.audio_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_dto = static_cast<CompanyOperatorAudioOutput*>(out_sp.get());
  EXPECT_EQ(out_dto->request_id, 6001u);
  EXPECT_EQ(out_dto->status_code, 0);
  ASSERT_NE(out_dto->transcribed_text, nullptr);
  ASSERT_NE(out_dto->intent_slot_json, nullptr);
  EXPECT_GT(out_dto->transcribed_text->length, 0);
  EXPECT_GT(out_dto->intent_slot_json->length, 0);

  outputs.clear();
  out_sp.reset();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// Golden Test 7: CrossRerank (Biz 7)
TEST_F(OperatorGoldenTest, CrossRerankGolden) {
  using namespace llm_edgeflow::operator_api;
  CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_cross_rerank.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kCpu;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::string query = "如何部署 EdgeFlow";
  std::string passage1 = "EdgeFlow 部署指南";
  std::string passage2 = "今天天气真好";
  CompanyString q_cs{static_cast<int32_t>(query.size()),
                     const_cast<char*>(query.data())};
  CompanyString p1_cs{static_cast<int32_t>(passage1.size()),
                      const_cast<char*>(passage1.data())};
  CompanyString p2_cs{static_cast<int32_t>(passage2.size()),
                      const_cast<char*>(passage2.data())};

  CompanyOperatorRerankInput in{};
  in.request_id = 7001;
  in.query_text = &q_cs;
  in.candidate_passages[0] = &p1_cs;
  in.candidate_passages[1] = &p2_cs;
  in.candidate_count = 2;

  NamedIoBatch inputs(1);
  inputs[0]["ranker.rerank_in"] = MakeBorrowedOperatorInput(&in);

  NamedIoBatch outputs(1);
  outputs[0]["ranker.rerank_out"] = std::shared_ptr<void>();

  ASSERT_EQ(ops_.Process(handle, inputs, outputs), 0);
  auto out_sp = outputs[0]["ranker.rerank_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_dto = static_cast<CompanyOperatorRerankOutput*>(out_sp.get());
  EXPECT_EQ(out_dto->request_id, 7001u);
  EXPECT_GT(out_dto->count, 0);

  outputs.clear();
  out_sp.reset();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

}  // namespace alg_framework

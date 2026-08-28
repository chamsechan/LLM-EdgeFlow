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

// Golden Test 1: DocQA (Biz 1)
TEST_F(OperatorGoldenTest, DocQaGolden) {
  using namespace llm_edgeflow::operator_api;
  CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_doc_qa.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kCpu;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::string doc =
      "LLM-EdgeFlow 采用 C++ 实现高性能端侧推理架构与 Pipeline 调度设计。";
  std::string query = "LLM-EdgeFlow 的架构设计是什么？";
  CompanyString cs_doc{static_cast<int32_t>(doc.size()),
                       const_cast<char*>(doc.data())};
  CompanyString cs_query{static_cast<int32_t>(query.size()),
                         const_cast<char*>(query.data())};

  CompanyOperatorDocInput in{};
  in.request_id = 101;
  in.doc_text = &cs_doc;
  in.query_text = &cs_query;

  NamedIoBatch inputs(1);
  inputs[0]["rag_channel.doc_in"] = MakeBorrowedOperatorInput(&in);

  NamedIoBatch outputs(1);
  outputs[0]["rag_channel.doc_out"] = std::shared_ptr<void>();

  int p_ret = ops_.Process(handle, inputs, outputs);
  ASSERT_EQ(p_ret, 0) << "Process error: "
                      << llm_edgeflow::operator_api::GetOperatorLastError();
  auto out_sp = outputs[0]["rag_channel.doc_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_dto = static_cast<CompanyOperatorDocOutput*>(out_sp.get());
  EXPECT_EQ(out_dto->request_id, 101u);
  EXPECT_EQ(out_dto->status_code, 0);
  ASSERT_NE(out_dto->intent_name, nullptr);
  EXPECT_STREQ(out_dto->intent_name->data, "TECH_ARCHITECTURE");
  EXPECT_GT(out_dto->confidence, 0.5f);
  EXPECT_GT(out_dto->chunk_count, 0);
  ASSERT_NE(out_dto->answer_text, nullptr);
  EXPECT_GT(out_dto->answer_text->length, 0);

  outputs.clear();
  out_sp.reset();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// Golden Test 2: KeywordMatch (Biz 2)
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

// Golden Test 3: EntityExtract (Biz 3)
TEST_F(OperatorGoldenTest, EntityExtractGolden) {
  using namespace llm_edgeflow::operator_api;
  CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_entity_extract.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kCpu;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::string text = "张三在北京大学计算机学院攻读博士学位。";
  CompanyString cs_text{static_cast<int32_t>(text.size()),
                        const_cast<char*>(text.data())};

  CompanyOperatorEntityInput in{};
  in.request_id = 3001;
  in.sentence_text = &cs_text;

  NamedIoBatch inputs(1);
  inputs[0]["ner_channel.entity_in"] = MakeBorrowedOperatorInput(&in);

  NamedIoBatch outputs(1);
  outputs[0]["ner_channel.entity_out"] = std::shared_ptr<void>();

  int p_ret = ops_.Process(handle, inputs, outputs);
  ASSERT_EQ(p_ret, 0) << "Process error: "
                      << llm_edgeflow::operator_api::GetOperatorLastError();
  auto out_sp = outputs[0]["ner_channel.entity_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_dto = static_cast<CompanyOperatorEntityOutput*>(out_sp.get());
  EXPECT_EQ(out_dto->request_id, 3001u);
  ASSERT_NE(out_dto->entities_json, nullptr);
  EXPECT_GT(out_dto->entities_json->length, 0);

  outputs.clear();
  out_sp.reset();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// Golden Test 4: ComplianceAudit (Biz 4)
TEST_F(OperatorGoldenTest, ComplianceAuditGolden) {
  using namespace llm_edgeflow::operator_api;
  CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_dialogue_audit.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kCpu;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::string user_text = "请加我个人微信私下转账，可以给您优惠";
  std::string channel = "chat_01";
  CompanyString cs_user{static_cast<int32_t>(user_text.size()),
                        const_cast<char*>(user_text.data())};
  CompanyString cs_chan{static_cast<int32_t>(channel.size()),
                        const_cast<char*>(channel.data())};

  CompanyOperatorAuditInput in{};
  in.request_id = 4001;
  in.user_text = &cs_user;
  in.channel_name = &cs_chan;

  NamedIoBatch inputs(1);
  inputs[0]["audit_channel.audit_in"] = MakeBorrowedOperatorInput(&in);

  NamedIoBatch outputs(1);
  outputs[0]["audit_channel.audit_out"] = std::shared_ptr<void>();

  int p_ret = ops_.Process(handle, inputs, outputs);
  ASSERT_EQ(p_ret, 0) << "Process error: "
                      << llm_edgeflow::operator_api::GetOperatorLastError();
  auto out_sp = outputs[0]["audit_channel.audit_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_dto = static_cast<CompanyOperatorAuditOutput*>(out_sp.get());
  EXPECT_EQ(out_dto->request_id, 4001u);
  EXPECT_EQ(out_dto->status_code, 0);
  ASSERT_NE(out_dto->risk_level, nullptr);
  ASSERT_NE(out_dto->matched_policy_clause, nullptr);
  ASSERT_NE(out_dto->audit_verdict_json, nullptr);
  EXPECT_GT(out_dto->audit_verdict_json->length, 0);

  outputs.clear();
  out_sp.reset();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// Golden Test 5: OcrDocQA (Biz 5)
TEST_F(OperatorGoldenTest, OcrDocQaGolden) {
  using namespace llm_edgeflow::operator_api;
  CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_ocr_doc_qa.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kCpu;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::string img_path = "./data/invoice_sample.png";
  std::string query = "请提取发票代码和金额";
  CompanyString cs_img{static_cast<int32_t>(img_path.size()),
                       const_cast<char*>(img_path.data())};
  CompanyString cs_query{static_cast<int32_t>(query.size()),
                         const_cast<char*>(query.data())};

  CompanyFrame frame{5001, &cs_img, nullptr};

  NamedIoBatch inputs(1);
  inputs[0]["ocr_channel.frame"] = MakeBorrowedOperatorInput(&frame);
  inputs[0]["ocr_channel.string"] = MakeBorrowedOperatorInput(&cs_query);

  NamedIoBatch outputs(1);
  outputs[0]["ocr_channel.od_out"] = std::shared_ptr<void>();

  int p_ret = ops_.Process(handle, inputs, outputs);
  ASSERT_EQ(p_ret, 0) << "Process error: "
                      << llm_edgeflow::operator_api::GetOperatorLastError();
  auto out_sp = outputs[0]["ocr_channel.od_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_dto = static_cast<CompanyOdOutput*>(out_sp.get());
  EXPECT_EQ(out_dto->request_id, 5001u);
  EXPECT_GT(out_dto->detected_box_count, 0);
  ASSERT_NE(out_dto->result_json, nullptr);
  EXPECT_GT(out_dto->result_json->length, 0);

  outputs.clear();
  out_sp.reset();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// Golden Test 6: AudioAsrIntent (Biz 6) with Slot Extraction Exact Golden
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

  // Sample 1: Navigation with avoid traffic (sum > 50 in mock ASR)
  std::vector<float> pcm_nav(16000, 0.05f);  // sum = 800 > 50
  // Sample 2: HVAC temp and fan speed (sum <= 50 in mock ASR)
  std::vector<float> pcm_hvac(16000, 0.001f);  // sum = 16 <= 50

  CompanyOperatorAudioInput in1{6001, pcm_nav.data(),
                                static_cast<int32_t>(pcm_nav.size()), 16000};
  CompanyOperatorAudioInput in2{6002, pcm_hvac.data(),
                                static_cast<int32_t>(pcm_hvac.size()), 16000};

  NamedIoBatch inputs(2);
  inputs[0]["mic_0.audio_in"] = MakeBorrowedOperatorInput(&in1);
  inputs[1]["mic_0.audio_in"] = MakeBorrowedOperatorInput(&in2);

  NamedIoBatch outputs(2);
  outputs[0]["mic_0.audio_out"] = std::shared_ptr<void>();
  outputs[1]["mic_0.audio_out"] = std::shared_ptr<void>();

  int p_ret = ops_.Process(handle, inputs, outputs);
  ASSERT_EQ(p_ret, 0) << "Process error: "
                      << llm_edgeflow::operator_api::GetOperatorLastError();

  // Verify Sample 1: Navigation
  auto out_sp1 = outputs[0]["mic_0.audio_out"];
  ASSERT_NE(out_sp1, nullptr);
  auto* out_dto1 = static_cast<CompanyOperatorAudioOutput*>(out_sp1.get());
  EXPECT_EQ(out_dto1->request_id, 6001u);
  EXPECT_EQ(out_dto1->status_code, 0);
  ASSERT_NE(out_dto1->transcribed_text, nullptr);
  ASSERT_NE(out_dto1->intent_slot_json, nullptr);
  std::string transcript1(out_dto1->transcribed_text->data,
                          out_dto1->transcribed_text->length);
  EXPECT_STREQ(transcript1.c_str(), "帮我导航到清华科技园，避开拥堵路段。");

  std::string slot_str1(out_dto1->intent_slot_json->data,
                        out_dto1->intent_slot_json->length);
  auto j1 = nlohmann::json::parse(slot_str1);
  EXPECT_EQ(j1["intent"], "NAVIGATION");
  ASSERT_TRUE(j1.contains("slots"));
  EXPECT_EQ(j1["slots"]["destination"], "清华科技园");
  EXPECT_EQ(j1["slots"]["avoid_traffic"], "true");
  EXPECT_EQ(j1["slots"]["avoid_toll"], "false");

  // Verify Sample 2: HVAC Control
  auto out_sp2 = outputs[1]["mic_0.audio_out"];
  ASSERT_NE(out_sp2, nullptr);
  auto* out_dto2 = static_cast<CompanyOperatorAudioOutput*>(out_sp2.get());
  EXPECT_EQ(out_dto2->request_id, 6002u);
  EXPECT_EQ(out_dto2->status_code, 0);
  ASSERT_NE(out_dto2->transcribed_text, nullptr);
  ASSERT_NE(out_dto2->intent_slot_json, nullptr);
  std::string transcript2(out_dto2->transcribed_text->data,
                          out_dto2->transcribed_text->length);
  EXPECT_STREQ(transcript2.c_str(), "把空调温度调到24度，风量开到二档。");

  std::string slot_str2(out_dto2->intent_slot_json->data,
                        out_dto2->intent_slot_json->length);
  auto j2 = nlohmann::json::parse(slot_str2);
  EXPECT_EQ(j2["intent"], "VEHICLE_HVAC_CONTROL");
  ASSERT_TRUE(j2.contains("slots"));
  EXPECT_EQ(j2["slots"]["target_temp"], "24");
  EXPECT_EQ(j2["slots"]["fan_speed"], "二");

  outputs.clear();
  out_sp1.reset();
  out_sp2.reset();
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
  std::string passage1 = "EdgeFlow 部署指南与快速上手";
  std::string passage2 = "今天北京天气多云转晴";
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

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "adapter/platform/company_conf_resolver.h"
#include "adapter/platform/platform_business_bridge_registry.h"
#include "adapter/platform/platform_output_pool.h"
#include "adapter/platform/platform_value_type_registry.h"
#include "company_alg_interface.h"
#include "platform/company_platform_types.h"
#include "platform/platform_operator_interface.h"

using namespace llm_edgeflow::platform;

static std::string GetConfDir() {
  if (std::filesystem::exists("configs")) {
    return std::filesystem::current_path().string();
  }
  return std::filesystem::current_path().parent_path().string();
}

class ScopedTempDirectory {
 public:
  ScopedTempDirectory() {
    static std::atomic<uint64_t> sequence{0};
    path_ = std::filesystem::temp_directory_path() /
            ("llm_edgeflow_platform_test_" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "_" + std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }

  ~ScopedTempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

class PlatformOperatorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ops_ = Get_LLM_EDGEFLOW_OperatorTable();
    ASSERT_NE(ops_.Init, nullptr);
    ASSERT_NE(ops_.Create, nullptr);
    ASSERT_NE(ops_.Process, nullptr);
    ASSERT_NE(ops_.Control, nullptr);
    ASSERT_NE(ops_.Destroy, nullptr);
    ASSERT_NE(ops_.Deinit, nullptr);

    int ret = ops_.Init();
    ASSERT_EQ(ret, 0);
  }

  void TearDown() override {
    int ret = ops_.Deinit();
    EXPECT_EQ(ret, 0);
  }

  OperatorFunc ops_{};
};

// 1. 测试函数表完整性与空安全
TEST_F(PlatformOperatorTest, OperatorTableIntegrity) {
  OperatorFunc table = Get_LLM_EDGEFLOW_OperatorTable();
  EXPECT_NE(table.Init, nullptr);
  EXPECT_NE(table.Create, nullptr);
  EXPECT_NE(table.Process, nullptr);
  EXPECT_NE(table.Control, nullptr);
  EXPECT_NE(table.Destroy, nullptr);
  EXPECT_NE(table.Deinit, nullptr);
}

// 2. 参数校验与负向安全拦截 (Create 阶段)
TEST_F(PlatformOperatorTest, CreateParameterValidation) {
  void* handle = nullptr;
  std::string root_dir = GetConfDir();
  std::string rel_conf = "configs/pipeline_keyword_match.conf";

  // 1. 空 handle 指针
  EXPECT_EQ(ops_.Create(nullptr, nullptr), -1);

  // 2. *handle 非空
  void* dummy_ptr = reinterpret_cast<void*>(0x1234);
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = rel_conf.c_str();
  param.device_id = 0;
  param.platform_type = ChipType::kAx650;
  param.max_frame_depth = 25;
  EXPECT_EQ(ops_.Create(&dummy_ptr, &param), -1);

  // 3. 空 param
  handle = nullptr;
  EXPECT_EQ(ops_.Create(&handle, nullptr), -1);

  // 4. 空配置路径或空根目录
  param.cfg_file_name = nullptr;
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  param.cfg_file_name = "";
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  param.cfg_file_name = rel_conf.c_str();
  param.model_path = nullptr;
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  param.model_path = "";
  EXPECT_EQ(ops_.Create(&handle, &param), -2);

  // 5. cfg_file_name 传入绝对路径 -> 拒绝
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "/etc/passwd";
  EXPECT_EQ(ops_.Create(&handle, &param), -2);

  // 6. cfg_file_name 目录穿越逃逸 (..) -> 拒绝
  param.cfg_file_name = "../../../etc/passwd";
  EXPECT_EQ(ops_.Create(&handle, &param), -2);

  // 7. 非法 device_id < 0
  param.cfg_file_name = rel_conf.c_str();
  param.device_id = -1;
  EXPECT_EQ(ops_.Create(&handle, &param), -2);

  // 8. 未知芯片类型 ChipType::kUnknown 及非法枚举值
  param.device_id = 0;
  param.platform_type = ChipType::kUnknown;
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  param.platform_type = static_cast<ChipType>(9999);
  EXPECT_EQ(ops_.Create(&handle, &param), -2);

  // 9. 不存在的文件
  param.platform_type = ChipType::kAx650;
  param.cfg_file_name = "configs/non_existent_file.conf";
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  EXPECT_NE(GetPlatformLastError(), nullptr);
}

// 3. 强类型 Control 正常与边界异常测试
TEST_F(PlatformOperatorTest, StronglyTypedControlValidation) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match.conf";
  param.device_id = 0;
  param.platform_type = ChipType::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  // 3.1 ControlUpdateRulesParam 测试
  ControlUpdateRulesParam rules_param_null{nullptr};
  EXPECT_EQ(
      ops_.Control(handle, ControlCommand::kUpdateRules, &rules_param_null),
      -2);

  ControlUpdateRulesParam rules_param_invalid{"not_a_json_object"};
  EXPECT_EQ(
      ops_.Control(handle, ControlCommand::kUpdateRules, &rules_param_invalid),
      -2);

  ControlUpdateRulesParam rules_param_valid{
      "{\"categories\":{\"VIP_SERVICE\":[\"VIP\",\"加急\"]}}"};
  EXPECT_EQ(
      ops_.Control(handle, ControlCommand::kUpdateRules, &rules_param_valid),
      0);

  // 3.2 ControlSwitchPromptParam 测试
  ControlSwitchPromptParam prompt_param_null{"prompt_v1", nullptr};
  EXPECT_EQ(
      ops_.Control(handle, ControlCommand::kSwitchPrompt, &prompt_param_null),
      -2);

  ControlSwitchPromptParam prompt_param_valid{"prompt_v2",
                                              "用户提问：{query}，请回答："};
  EXPECT_EQ(
      ops_.Control(handle, ControlCommand::kSwitchPrompt, &prompt_param_valid),
      0);

  // 3.3 ControlUpdateThresholdParam 测试 (含 NaN / Infinity 特殊浮点数拦截)
  ControlUpdateThresholdParam thresh_low{"VIP_SERVICE", -0.1f};
  EXPECT_EQ(ops_.Control(handle, ControlCommand::kUpdateThreshold, &thresh_low),
            -2);
  ControlUpdateThresholdParam thresh_high{"VIP_SERVICE", 1.5f};
  EXPECT_EQ(
      ops_.Control(handle, ControlCommand::kUpdateThreshold, &thresh_high), -2);

  float nan_val = std::numeric_limits<float>::quiet_NaN();
  ControlUpdateThresholdParam thresh_nan{"VIP_SERVICE", nan_val};
  EXPECT_EQ(ops_.Control(handle, ControlCommand::kUpdateThreshold, &thresh_nan),
            -2);

  float inf_val = std::numeric_limits<float>::infinity();
  ControlUpdateThresholdParam thresh_inf{"VIP_SERVICE", inf_val};
  EXPECT_EQ(ops_.Control(handle, ControlCommand::kUpdateThreshold, &thresh_inf),
            -2);

  ControlUpdateThresholdParam thresh_valid{"VIP_SERVICE", 0.85f};
  EXPECT_EQ(
      ops_.Control(handle, ControlCommand::kUpdateThreshold, &thresh_valid), 0);

  // 3.4 未知命令枚举
  EXPECT_EQ(
      ops_.Control(handle, static_cast<ControlCommand>(999), &thresh_valid),
      -2);

  ops_.Destroy(handle);
}

// 4. 句柄生命周期与防护测试
TEST_F(PlatformOperatorTest, HandleLifecycleAndUafPrevention) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match.conf";
  param.device_id = 0;
  param.platform_type = ChipType::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  EXPECT_EQ(ops_.Destroy(handle), 0);
  EXPECT_EQ(ops_.Destroy(handle), -1);

  std::string text = "test";
  CompanyString cs{static_cast<int32_t>(text.size()),
                   const_cast<char*>(text.data())};
  CompanyPlatformKeywordInput in{101, &cs};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["chan.keyword_in"] = MakeBorrowedPlatformInput(&in);
  out_b[0]["chan.keyword_out"] = std::shared_ptr<void>();

  EXPECT_EQ(ops_.Process(handle, in_b, out_b), -1);
}

// 5. CompanyString 校验规则测试 (包含嵌入 NUL 拦截、负长度与超限拦截)
TEST_F(PlatformOperatorTest, CompanyStringValidation) {
  using namespace alg_framework;
  std::string err;

  // 1. null 指针
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyString(nullptr, 100,
                                                             "str", &err),
            -3);

  // 2. 负长度
  char buf[] = "hello";
  CompanyString cs_neg{-1, buf};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyString(&cs_neg, 100,
                                                             "str", &err),
            -3);

  // 3. 长度为 0 (正常空字符串)
  CompanyString cs_zero{0, nullptr};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyString(&cs_zero, 100,
                                                             "str", &err),
            0);

  // 4. 长度超限
  CompanyString cs_toolarge{150, buf};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyString(&cs_toolarge, 100,
                                                             "str", &err),
            -3);

  // 5. 长度 > 0 但 data == nullptr
  CompanyString cs_nulldata{10, nullptr};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyString(&cs_nulldata, 100,
                                                             "str", &err),
            -3);

  // 6. 嵌入 NUL 字符 (禁止)
  char embedded_nul[] = "hello\0world";
  CompanyString cs_embed{11, embedded_nul};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyString(&cs_embed, 100,
                                                             "str", &err),
            -3);

  // 7. 正常字符串
  CompanyString cs_valid{5, buf};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyString(&cs_valid, 100,
                                                             "str", &err),
            0);
}

// 6. 关注词匹配业务端到端 (Keyword Match)
TEST_F(PlatformOperatorTest, EndToEndKeywordMatch) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match.conf";
  param.device_id = 0;
  param.platform_type = ChipType::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  ControlUpdateRulesParam rules_param{
      "{\"categories\":{\"VIP_SERVICE\":[\"VIP\",\"加急\"]}}"};
  ASSERT_EQ(ops_.Control(handle, ControlCommand::kUpdateRules, &rules_param),
            0);

  std::string text = "请帮我联系VIP专员，加急处理";
  CompanyString cs{static_cast<int32_t>(text.size()),
                   const_cast<char*>(text.data())};
  CompanyPlatformKeywordInput in{1001, &cs};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["chan.keyword_in"] = MakeBorrowedPlatformInput(&in);
  out_b[0]["chan.keyword_out"] = std::shared_ptr<void>();

  ASSERT_EQ(ops_.Process(handle, in_b, out_b), 0);

  auto out_sp = out_b[0]["chan.keyword_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_ptr = static_cast<CompanyPlatformKeywordOutput*>(out_sp.get());
  EXPECT_EQ(out_ptr->request_id, 1001u);
  EXPECT_EQ(out_ptr->is_hit, 1);
  EXPECT_NE(out_ptr->match_result_json, nullptr);
  EXPECT_GT(out_ptr->match_result_json->length, 0);

  // 释放输出块，触发回池
  out_b.clear();
  out_sp.reset();

  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// 7. 多模态 OCR 业务多槽位聚合端到端 (frame + string -> od_out)
TEST_F(PlatformOperatorTest, EndToEndOcrDocQaMultiSlot) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_ocr_doc_qa.conf";
  param.device_id = 0;
  param.platform_type = ChipType::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::string uri = "./data/invoice_01.jpg";
  std::string prompt = "提取发票代码、号码与总金额";
  CompanyString uri_cs{static_cast<int32_t>(uri.size()),
                       const_cast<char*>(uri.data())};
  CompanyFrame frame{60001, &uri_cs, nullptr};
  CompanyString prompt_cs{static_cast<int32_t>(prompt.size()),
                          const_cast<char*>(prompt.data())};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["camera_0.frame"] = MakeBorrowedPlatformInput(&frame);
  in_b[0]["camera_0.string"] = MakeBorrowedPlatformInput(&prompt_cs);
  out_b[0]["camera_0.od_out"] = std::shared_ptr<void>();

  ASSERT_EQ(ops_.Process(handle, in_b, out_b), 0);

  auto out_sp = out_b[0]["camera_0.od_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_ptr = static_cast<CompanyOdOutput*>(out_sp.get());
  EXPECT_EQ(out_ptr->request_id, 60001u);
  EXPECT_GT(out_ptr->detected_box_count, 0);
  EXPECT_NE(out_ptr->result_json, nullptr);
  EXPECT_GT(out_ptr->result_json->length, 0);

  out_b.clear();
  out_sp.reset();

  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// 8. 智能长文档问答业务 (Doc QA)
TEST_F(PlatformOperatorTest, EndToEndDocQa) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_doc_qa.conf";
  param.device_id = 0;
  param.platform_type = ChipType::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::string doc = "企业级算法框架设计规范：采用4层分层架构。";
  std::string query = "请简述该算法框架的架构设计？";
  CompanyString doc_cs{static_cast<int32_t>(doc.size()),
                       const_cast<char*>(doc.data())};
  CompanyString query_cs{static_cast<int32_t>(query.size()),
                         const_cast<char*>(query.data())};
  CompanyPlatformDocInput in{10001, &doc_cs, &query_cs};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["rag_channel.doc_in"] = MakeBorrowedPlatformInput(&in);
  out_b[0]["rag_channel.doc_out"] = std::shared_ptr<void>();

  ASSERT_EQ(ops_.Process(handle, in_b, out_b), 0);

  auto out_sp = out_b[0]["rag_channel.doc_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_ptr = static_cast<CompanyPlatformDocOutput*>(out_sp.get());
  EXPECT_EQ(out_ptr->request_id, 10001u);
  EXPECT_NE(out_ptr->answer_text, nullptr);
  EXPECT_GT(out_ptr->answer_text->length, 0);

  out_b.clear();
  out_sp.reset();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// 9. 智能对话风控质检业务 (Compliance Audit)
TEST_F(PlatformOperatorTest, EndToEndComplianceAudit) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_dialogue_audit.conf";
  param.device_id = 0;
  param.platform_type = ChipType::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::string chan = "VIP专席客服";
  std::string dialogue = "亲，加我私人微信转账，私下寄给你返现20元！";
  CompanyString chan_cs{static_cast<int32_t>(chan.size()),
                        const_cast<char*>(chan.data())};
  CompanyString dia_cs{static_cast<int32_t>(dialogue.size()),
                       const_cast<char*>(dialogue.data())};
  CompanyPlatformAuditInput in{40001, &dia_cs, &chan_cs};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["audit_channel.audit_in"] = MakeBorrowedPlatformInput(&in);
  out_b[0]["audit_channel.audit_out"] = std::shared_ptr<void>();

  ASSERT_EQ(ops_.Process(handle, in_b, out_b), 0);

  auto out_sp = out_b[0]["audit_channel.audit_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_ptr = static_cast<CompanyPlatformAuditOutput*>(out_sp.get());
  EXPECT_EQ(out_ptr->request_id, 40001u);
  EXPECT_NE(out_ptr->risk_level, nullptr);
  EXPECT_GT(out_ptr->risk_level->length, 0);

  out_b.clear();
  out_sp.reset();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// 10. 语音识别与意图抽取业务 (Audio ASR Intent)
TEST_F(PlatformOperatorTest, EndToEndAudioAsrIntent) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_audio_asr_intent.conf";
  param.device_id = 0;
  param.platform_type = ChipType::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::vector<float> pcm(16000, 0.01f);
  CompanyPlatformAudioInput in{70001, pcm.data(),
                               static_cast<int32_t>(pcm.size()), 16000};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["mic_0.audio_in"] = MakeBorrowedPlatformInput(&in);
  out_b[0]["mic_0.audio_out"] = std::shared_ptr<void>();

  ASSERT_EQ(ops_.Process(handle, in_b, out_b), 0);

  auto out_sp = out_b[0]["mic_0.audio_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_ptr = static_cast<CompanyPlatformAudioOutput*>(out_sp.get());
  EXPECT_EQ(out_ptr->request_id, 70001u);
  EXPECT_NE(out_ptr->transcribed_text, nullptr);
  EXPECT_GT(out_ptr->transcribed_text->length, 0);

  out_b.clear();
  out_sp.reset();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// 11. 纯语义精排业务 (Cross Rerank)
TEST_F(PlatformOperatorTest, EndToEndCrossRerank) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_cross_rerank.conf";
  param.device_id = 0;
  param.platform_type = ChipType::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::string query = "怎么办理7天无理由退款？";
  std::string passage1 = "条款A: 境外交易加收3%手续费。";
  std::string passage2 = "条款B: 售后退款支持7天无理由。";
  CompanyString query_cs{static_cast<int32_t>(query.size()),
                         const_cast<char*>(query.data())};
  CompanyString p1_cs{static_cast<int32_t>(passage1.size()),
                      const_cast<char*>(passage1.data())};
  CompanyString p2_cs{static_cast<int32_t>(passage2.size()),
                      const_cast<char*>(passage2.data())};

  CompanyPlatformRerankInput in{};
  in.request_id = 80001;
  in.query_text = &query_cs;
  in.candidate_passages[0] = &p1_cs;
  in.candidate_passages[1] = &p2_cs;
  in.candidate_count = 2;

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["ranker.rerank_in"] = MakeBorrowedPlatformInput(&in);
  out_b[0]["ranker.rerank_out"] = std::shared_ptr<void>();

  ASSERT_EQ(ops_.Process(handle, in_b, out_b), 0);

  auto out_sp = out_b[0]["ranker.rerank_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_ptr = static_cast<CompanyPlatformRerankOutput*>(out_sp.get());
  EXPECT_EQ(out_ptr->request_id, 80001u);
  EXPECT_EQ(out_ptr->count, 2);

  out_b.clear();
  out_sp.reset();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// 12. 输出占位非空拦截与未知 Key 拦截
TEST_F(PlatformOperatorTest, OutputSlotValidation) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match.conf";
  param.device_id = 0;
  param.platform_type = ChipType::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);

  std::string text = "test";
  CompanyString cs{static_cast<int32_t>(text.size()),
                   const_cast<char*>(text.data())};
  CompanyPlatformKeywordInput in{1001, &cs};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["chan.keyword_in"] = MakeBorrowedPlatformInput(&in);

  // 1. 输出槽位为非空 shared_ptr -> 拦截返回 -4
  CompanyPlatformKeywordOutput dummy_out{};
  out_b[0]["chan.keyword_out"] =
      std::shared_ptr<void>(&dummy_out, [](void*) {});
  EXPECT_EQ(ops_.Process(handle, in_b, out_b), -4);

  // 2. 缺少输出槽位 Key -> 拦截返回 -4
  out_b[0].clear();
  EXPECT_EQ(ops_.Process(handle, in_b, out_b), -4);

  // 3. 包含额外未知输出槽位 Key -> 拦截返回 -4
  out_b[0]["chan.keyword_out"] = std::shared_ptr<void>();
  out_b[0]["chan.extra_key"] = std::shared_ptr<void>();
  EXPECT_EQ(ops_.Process(handle, in_b, out_b), -4);

  ops_.Destroy(handle);
}

// 13. 输出池耗尽、阻塞与唤醒复用测试
TEST_F(PlatformOperatorTest, OutputPoolExhaustionAndBlocking) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match.conf";
  param.device_id = 0;
  param.platform_type = ChipType::kAx650;
  param.max_frame_depth = 2;  // 设定极小深度 2

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);

  std::string text = "test";
  CompanyString cs{static_cast<int32_t>(text.size()),
                   const_cast<char*>(text.data())};
  CompanyPlatformKeywordInput in{1001, &cs};

  // 1. 单次 Batch > max_frame_depth -> 立即拒绝 (-3)，不陷入死锁
  NamedIoBatch in_b3(3), out_b3(3);
  for (int i = 0; i < 3; ++i) {
    in_b3[i]["chan.keyword_in"] = MakeBorrowedPlatformInput(&in);
    out_b3[i]["chan.keyword_out"] = std::shared_ptr<void>();
  }
  EXPECT_EQ(ops_.Process(handle, in_b3, out_b3), -3);

  // 2. 连续检出 2 个块，暂不释放
  NamedIoBatch in_b1(1), out_b1(1);
  in_b1[0]["chan.keyword_in"] = MakeBorrowedPlatformInput(&in);
  out_b1[0]["chan.keyword_out"] = std::shared_ptr<void>();
  ASSERT_EQ(ops_.Process(handle, in_b1, out_b1), 0);
  auto out1 = out_b1[0]["chan.keyword_out"];

  NamedIoBatch in_b2(1), out_b2(1);
  in_b2[0]["chan.keyword_in"] = MakeBorrowedPlatformInput(&in);
  out_b2[0]["chan.keyword_out"] = std::shared_ptr<void>();
  ASSERT_EQ(ops_.Process(handle, in_b2, out_b2), 0);
  auto out2 = out_b2[0]["chan.keyword_out"];

  // 此时池中可用块为 0
  std::atomic<bool> thread_started{false};
  std::atomic<bool> thread_completed{false};

  std::thread worker([&]() {
    thread_started = true;
    NamedIoBatch in_b(1), out_b(1);
    in_b[0]["chan.keyword_in"] = MakeBorrowedPlatformInput(&in);
    out_b[0]["chan.keyword_out"] = std::shared_ptr<void>();
    int ret = ops_.Process(handle, in_b, out_b);
    EXPECT_EQ(ret, 0);
    thread_completed = true;
  });

  while (!thread_started) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  // 此时应当处于阻塞状态
  EXPECT_FALSE(thread_completed);

  // 释放一个旧输出，唤醒阻塞线程
  out1.reset();
  out_b1.clear();

  worker.join();
  EXPECT_TRUE(thread_completed);

  out2.reset();
  out_b2.clear();

  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// 14. 违约场景 Destroy：仍有检出块时安全清理并返回 -1
TEST_F(PlatformOperatorTest, DestroyViolationHandling) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match.conf";
  param.device_id = 0;
  param.platform_type = ChipType::kAx650;
  param.max_frame_depth = 5;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);

  std::string text = "test";
  CompanyString cs{static_cast<int32_t>(text.size()),
                   const_cast<char*>(text.data())};
  CompanyPlatformKeywordInput in{1001, &cs};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["chan.keyword_in"] = MakeBorrowedPlatformInput(&in);
  out_b[0]["chan.keyword_out"] = std::shared_ptr<void>();
  ASSERT_EQ(ops_.Process(handle, in_b, out_b), 0);

  auto leak_out = out_b[0]["chan.keyword_out"];

  // 调用方违约在未释放输出时调用 Destroy -> 返回 -1 且清理资源
  EXPECT_EQ(ops_.Destroy(handle), -1);

  // 句柄已被消费，重复 Destroy 返回 -1
  EXPECT_EQ(ops_.Destroy(handle), -1);

  // 违约持有的 shared_ptr 析构时通过 weak token 安全 no-op，不崩溃
  leak_out.reset();
}

// 15. ValidatePlatformConfigBinding 双路径校验接口测试
TEST_F(PlatformOperatorTest, ValidatePlatformConfigBindingApi) {
  std::string root_dir = GetConfDir();
  char err_buf[256] = {0};

  // 1. 正常校验
  EXPECT_EQ(ValidatePlatformConfigBinding(
                root_dir.c_str(), "configs/pipeline_keyword_match.conf",
                static_cast<int32_t>(ALG_BIZ_TYPE_KEYWORD_MATCH), err_buf,
                sizeof(err_buf)),
            0);

  // 2. 业务不匹配
  EXPECT_EQ(
      ValidatePlatformConfigBinding(
          root_dir.c_str(), "configs/pipeline_keyword_match.conf",
          static_cast<int32_t>(ALG_BIZ_TYPE_DOC_QA), err_buf, sizeof(err_buf)),
      -3);

  // 3. 相对路径传入绝对路径 / 逃逸
  EXPECT_EQ(ValidatePlatformConfigBinding(
                root_dir.c_str(), "/etc/passwd",
                static_cast<int32_t>(ALG_BIZ_TYPE_KEYWORD_MATCH), err_buf,
                sizeof(err_buf)),
            -2);
}

// 16. 实体抽取业务端到端 (Entity Extract)
TEST_F(PlatformOperatorTest, EndToEndEntityExtract) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_entity_extract.conf";
  param.device_id = 0;
  param.platform_type = ChipType::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::string text = "张三在清华大学研发深度学习大模型。";
  CompanyString cs{static_cast<int32_t>(text.size()),
                   const_cast<char*>(text.data())};
  CompanyPlatformEntityInput in{30001, &cs};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["nlp.entity_in"] = MakeBorrowedPlatformInput(&in);
  out_b[0]["nlp.entity_out"] = std::shared_ptr<void>();

  ASSERT_EQ(ops_.Process(handle, in_b, out_b), 0);

  auto out_sp = out_b[0]["nlp.entity_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_ptr = static_cast<CompanyPlatformEntityOutput*>(out_sp.get());
  EXPECT_EQ(out_ptr->request_id, 30001u);
  EXPECT_NE(out_ptr->entities_json, nullptr);
  EXPECT_GT(out_ptr->entities_json->length, 0);

  out_b.clear();
  out_sp.reset();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// 17. CompanyBuffer 与 CompanyAny 平台值类型校验测试
TEST_F(PlatformOperatorTest, CompanyBufferAndAnyValidation) {
  using namespace alg_framework;
  const auto* buf_binding =
      PlatformValueTypeRegistry::Instance().GetBindingBySuffix("buffer");
  ASSERT_NE(buf_binding, nullptr);
  ASSERT_TRUE(buf_binding->validate_external);

  ResolvedInputLimits limits;
  std::string err;

  // CompanyBuffer: null pointer
  EXPECT_EQ(buf_binding->validate_external(nullptr, limits, &err), -3);

  // CompanyBuffer: negative length
  uint8_t dummy_data[] = {0x01, 0x02, 0x03};
  CompanyBuffer buf_neg{-1, dummy_data};
  EXPECT_EQ(buf_binding->validate_external(&buf_neg, limits, &err), -3);

  // CompanyBuffer: length > max
  CompanyBuffer buf_toolarge{static_cast<int32_t>(limits.max_buffer_bytes + 1),
                             dummy_data};
  EXPECT_EQ(buf_binding->validate_external(&buf_toolarge, limits, &err), -3);

  // CompanyBuffer: length > 0 with null data
  CompanyBuffer buf_nulldata{10, nullptr};
  EXPECT_EQ(buf_binding->validate_external(&buf_nulldata, limits, &err), -3);

  // CompanyBuffer: valid binary
  CompanyBuffer buf_valid{3, dummy_data};
  EXPECT_EQ(buf_binding->validate_external(&buf_valid, limits, &err), 0);

  // CompanyAny
  const auto* any_binding =
      PlatformValueTypeRegistry::Instance().GetBindingBySuffix("any");
  ASSERT_NE(any_binding, nullptr);
  ASSERT_TRUE(any_binding->validate_external);

  // CompanyAny: null pointer
  EXPECT_EQ(any_binding->validate_external(nullptr, limits, &err), -3);

  // CompanyAny: negative count / length
  CompanyAny any_neg{1, -1, 10, dummy_data};
  EXPECT_EQ(any_binding->validate_external(&any_neg, limits, &err), -3);

  // CompanyAny: byte_length > max
  CompanyAny any_toolarge{1, 10, static_cast<int32_t>(limits.max_any_bytes + 1),
                          dummy_data};
  EXPECT_EQ(any_binding->validate_external(&any_toolarge, limits, &err), -3);

  // 7. 正确尺寸方程: float32 (type_id=1), count=3, byte_length=12 -> 0
  CompanyAny any_valid{1, 3, 12, dummy_data};
  EXPECT_EQ(any_binding->validate_external(&any_valid, limits, &err), 0);
}

// 18. 输入 shared_ptr 所有权不持有与 use_count 校验测试
TEST_F(PlatformOperatorTest, InputSharedPtrUseCountNotRetained) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match.conf";
  param.device_id = 0;
  param.platform_type = ChipType::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);

  std::string text = "VIP专员";
  CompanyString cs{static_cast<int32_t>(text.size()),
                   const_cast<char*>(text.data())};
  auto in_ptr = std::make_shared<CompanyPlatformKeywordInput>();
  in_ptr->request_id = 1001;
  in_ptr->sentence_text = &cs;

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["chan.keyword_in"] = in_ptr;
  out_b[0]["chan.keyword_out"] = std::shared_ptr<void>();

  // Before process: in_ptr is held by in_ptr and in_b[0] (use_count == 2)
  EXPECT_EQ(in_ptr.use_count(), 2);

  ASSERT_EQ(ops_.Process(handle, in_b, out_b), 0);

  // After process: in_ptr is still held only by in_ptr and in_b[0] (use_count
  // == 2)
  EXPECT_EQ(in_ptr.use_count(), 2);

  in_b.clear();
  // Now only in_ptr holds it (use_count == 1)
  EXPECT_EQ(in_ptr.use_count(), 1);

  out_b.clear();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// 19. 输出内存池深度 0 归一化与地址复用校验测试
TEST_F(PlatformOperatorTest, OutputAddressReuseAndDepthNormalization) {
  std::string root_dir = GetConfDir();

  // 1. 深度 0 自动归一化为默认 25 测试
  {
    CreateParam param{};
    param.model_path = root_dir.c_str();
    param.cfg_file_name = "configs/pipeline_keyword_match.conf";
    param.device_id = 0;
    param.platform_type = ChipType::kAx650;
    param.max_frame_depth = 0;  // 0 应被自动归一化为默认 25

    void* handle = nullptr;
    ASSERT_EQ(ops_.Create(&handle, &param), 0);

    std::string text = "VIP专员";
    CompanyString cs{static_cast<int32_t>(text.size()),
                     const_cast<char*>(text.data())};
    CompanyPlatformKeywordInput in{1001, &cs};

    NamedIoBatch in_b(1), out_b(1);
    in_b[0]["chan.keyword_in"] = MakeBorrowedPlatformInput(&in);
    out_b[0]["chan.keyword_out"] = std::shared_ptr<void>();
    ASSERT_EQ(ops_.Process(handle, in_b, out_b), 0);
    EXPECT_NE(out_b[0]["chan.keyword_out"], nullptr);

    out_b.clear();
    EXPECT_EQ(ops_.Destroy(handle), 0);
  }

  // 2. 深度 1 的单一缓冲块复用验证
  {
    CreateParam param{};
    param.model_path = root_dir.c_str();
    param.cfg_file_name = "configs/pipeline_keyword_match.conf";
    param.device_id = 0;
    param.platform_type = ChipType::kAx650;
    param.max_frame_depth = 1;

    void* handle = nullptr;
    ASSERT_EQ(ops_.Create(&handle, &param), 0);

    std::string text = "VIP专员";
    CompanyString cs{static_cast<int32_t>(text.size()),
                     const_cast<char*>(text.data())};
    CompanyPlatformKeywordInput in{1001, &cs};

    NamedIoBatch in_b1(1), out_b1(1);
    in_b1[0]["chan.keyword_in"] = MakeBorrowedPlatformInput(&in);
    out_b1[0]["chan.keyword_out"] = std::shared_ptr<void>();
    ASSERT_EQ(ops_.Process(handle, in_b1, out_b1), 0);

    void* first_block_addr = out_b1[0]["chan.keyword_out"].get();
    ASSERT_NE(first_block_addr, nullptr);

    // 释放输出块，触发回池
    out_b1.clear();

    // 第二次调用，池中唯一的块必须被严格复用
    NamedIoBatch in_b2(1), out_b2(1);
    in_b2[0]["chan.keyword_in"] = MakeBorrowedPlatformInput(&in);
    out_b2[0]["chan.keyword_out"] = std::shared_ptr<void>();
    ASSERT_EQ(ops_.Process(handle, in_b2, out_b2), 0);

    void* second_block_addr = out_b2[0]["chan.keyword_out"].get();
    EXPECT_EQ(first_block_addr, second_block_addr);

    out_b2.clear();
    EXPECT_EQ(ops_.Destroy(handle), 0);
  }
}

// 20. 多句柄并发执行与独立输出池隔离测试
TEST_F(PlatformOperatorTest, ConcurrentDifferentHandles) {
  std::string root_dir = GetConfDir();
  CreateParam param1{};
  param1.model_path = root_dir.c_str();
  param1.cfg_file_name = "configs/pipeline_keyword_match.conf";
  param1.device_id = 0;
  param1.platform_type = ChipType::kAx650;
  param1.max_frame_depth = 10;

  CreateParam param2 = param1;

  void* handle1 = nullptr;
  void* handle2 = nullptr;
  ASSERT_EQ(ops_.Create(&handle1, &param1), 0);
  ASSERT_EQ(ops_.Create(&handle2, &param2), 0);
  ASSERT_NE(handle1, nullptr);
  ASSERT_NE(handle2, nullptr);
  ASSERT_NE(handle1, handle2);

  std::string text = "VIP专员";
  CompanyString cs{static_cast<int32_t>(text.size()),
                   const_cast<char*>(text.data())};
  CompanyPlatformKeywordInput in{1001, &cs};

  std::atomic<bool> success1{false};
  std::atomic<bool> success2{false};

  std::thread t1([&]() {
    for (int i = 0; i < 10; ++i) {
      NamedIoBatch in_b(1), out_b(1);
      in_b[0]["chan.keyword_in"] = MakeBorrowedPlatformInput(&in);
      out_b[0]["chan.keyword_out"] = std::shared_ptr<void>();
      if (ops_.Process(handle1, in_b, out_b) != 0) return;
      out_b.clear();
    }
    success1 = true;
  });

  std::thread t2([&]() {
    for (int i = 0; i < 10; ++i) {
      NamedIoBatch in_b(1), out_b(1);
      in_b[0]["chan.keyword_in"] = MakeBorrowedPlatformInput(&in);
      out_b[0]["chan.keyword_out"] = std::shared_ptr<void>();
      if (ops_.Process(handle2, in_b, out_b) != 0) return;
      out_b.clear();
    }
    success2 = true;
  });

  t1.join();
  t2.join();

  EXPECT_TRUE(success1);
  EXPECT_TRUE(success2);

  EXPECT_EQ(ops_.Destroy(handle1), 0);
  EXPECT_EQ(ops_.Destroy(handle2), 0);
}

// 21. mem_que 配置校验与异常 Fail-Closed 测试
TEST_F(PlatformOperatorTest, MemQueConfigValidationFailClosed) {
  ScopedTempDirectory temp_dir;
  std::filesystem::path root = temp_dir.path();
  std::filesystem::create_directories(root / "configs");
  std::filesystem::copy_file(std::filesystem::path(GetConfDir()) /
                                 "configs/pipeline_keyword_match.json",
                             root / "configs/pipeline_keyword_match.json");

  std::string conf_file = "configs/test.conf";
  std::filesystem::path conf_path = root / conf_file;

  // 1. 缺失 mem_que 对象 -> -2
  {
    std::ofstream ofs(conf_path);
    ofs << R"({
      "data": {
        "pipe_path": "configs/pipeline_keyword_match.json"
      }
    })";
  }
  const std::string root_str = root.string();
  CreateParam param{};
  param.model_path = root_str.c_str();
  param.cfg_file_name = conf_file.c_str();
  param.device_id = 0;
  param.platform_type = ChipType::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  EXPECT_EQ(ops_.Create(&handle, &param), -2);

  // 2. mem_que.type 与业务不匹配 -> -2
  {
    std::ofstream ofs(conf_path);
    ofs << R"({
      "data": {
        "pipe_path": "configs/pipeline_keyword_match.json",
        "mem_que": {
          "type": "doc_out"
        }
      }
    })";
  }
  EXPECT_EQ(ops_.Create(&handle, &param), -2);

  // 3. meta_num == 0 但 metadata_type_id != 0 -> -2
  {
    std::ofstream ofs(conf_path);
    ofs << R"({
      "data": {
        "pipe_path": "configs/pipeline_keyword_match.json",
        "mem_que": {
          "type": "keyword_out",
          "meta_num": 0,
          "metadata_type_id": 123
        }
      }
    })";
  }
  EXPECT_EQ(ops_.Create(&handle, &param), -2);

  // 4. 未知 capacity 字段 -> -2
  {
    std::ofstream ofs(conf_path);
    ofs << R"({
      "data": {
        "pipe_path": "configs/pipeline_keyword_match.json",
        "mem_que": {
          "type": "keyword_out",
          "capacities": {
            "unknown_field_xyz": 100
          }
        }
      }
    })";
  }
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
}

// 22. SSO 短字符串 (1~7 字节) 与跨批次指针绝对地址稳定性测试 (R9-001)
TEST_F(PlatformOperatorTest, ShortStringSsoAndAddressStability) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match.conf";
  param.device_id = 0;
  param.platform_type = ChipType::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  // 构造包含 1~7 字节短字符串 (SSO 敏感) 的多个样本
  const char* short_words[] = {"a",     "bc",     "def",    "ghij",
                               "klmno", "pqrstu", "vwxyz12"};
  NamedIoBatch batch_inputs(7);
  std::vector<CompanyString> company_strings(7);
  std::vector<CompanyPlatformKeywordInput> inputs(7);

  for (size_t i = 0; i < 7; ++i) {
    company_strings[i].length =
        static_cast<int32_t>(std::strlen(short_words[i]));
    company_strings[i].data = const_cast<char*>(short_words[i]);
    inputs[i].request_id = 70000 + i;
    inputs[i].sentence_text = &company_strings[i];

    batch_inputs[i]["client_channel.keyword_in"] =
        MakeBorrowedPlatformInput(&inputs[i]);
  }

  NamedIoBatch batch_outputs(7);
  for (size_t i = 0; i < 7; ++i) {
    batch_outputs[i]["client_channel.keyword_out"] = nullptr;
  }

  int ret = ops_.Process(handle, batch_inputs, batch_outputs);
  EXPECT_EQ(ret, 0);

  for (size_t i = 0; i < 7; ++i) {
    auto out_sp = batch_outputs[i]["client_channel.keyword_out"];
    ASSERT_NE(out_sp, nullptr);
    const auto* out =
        static_cast<const CompanyPlatformKeywordOutput*>(out_sp.get());
    EXPECT_EQ(out->request_id, 70000 + i);
  }

  ops_.Destroy(handle);
}

// 23. 控制块构造失败注入与两阶段原子发布回滚测试 (R9-002)
TEST_F(PlatformOperatorTest, AtomicPublishFailureRollback) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match.conf";
  param.device_id = 0;
  param.platform_type = ChipType::kAx650;
  param.max_frame_depth = 5;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  char sent1[] = "VIP";
  CompanyString cs1{3, sent1};
  CompanyPlatformKeywordInput in1{80001, &cs1};

  char sent2[] = "urgent";
  CompanyString cs2{6, sent2};
  CompanyPlatformKeywordInput in2{80002, &cs2};

  NamedIoBatch batch_inputs(2);
  batch_inputs[0]["client_channel.keyword_in"] =
      MakeBorrowedPlatformInput(&in1);
  batch_inputs[1]["client_channel.keyword_in"] =
      MakeBorrowedPlatformInput(&in2);

  NamedIoBatch batch_outputs(2);
  batch_outputs[0]["client_channel.keyword_out"] = nullptr;
  batch_outputs[1]["client_channel.keyword_out"] = nullptr;

  // 注入探针：在发布第 2 个控制块 (index=1) 时模拟 bad_alloc 异常
  alg_framework::OutputPoolState::SetPublishFailureCountdown(1);

  int ret = ops_.Process(handle, batch_inputs, batch_outputs);
  // 必须返回异常捕获错误 -99
  EXPECT_EQ(ret, -99);

  // 验证两阶段发布原子性：任何一个失败，对外输出必须全部保持 null
  EXPECT_EQ(batch_outputs[0]["client_channel.keyword_out"], nullptr);
  EXPECT_EQ(batch_outputs[1]["client_channel.keyword_out"], nullptr);

  // 验证池中块全量安全回退：下一次正常请求仍可完整获取所有块
  alg_framework::OutputPoolState::SetPublishFailureCountdown(-1);
  ret = ops_.Process(handle, batch_inputs, batch_outputs);
  EXPECT_EQ(ret, 0);
  EXPECT_NE(batch_outputs[0]["client_channel.keyword_out"], nullptr);
  EXPECT_NE(batch_outputs[1]["client_channel.keyword_out"], nullptr);

  ops_.Destroy(handle);
}

// 24. 严格路径沙箱与非法路径拦截测试 (R9-003)
TEST_F(PlatformOperatorTest, PathSandboxStrictBoundaries) {
  std::string root_dir = GetConfDir();
  char err_buf[256] = {0};

  // 1. POSIX 绝对路径拒绝
  EXPECT_EQ(ValidatePlatformConfigBinding(
                root_dir.c_str(), "/etc/pipeline.conf",
                static_cast<int32_t>(ALG_BIZ_TYPE_KEYWORD_MATCH), err_buf,
                sizeof(err_buf)),
            -2);

  // 2. Windows 盘符拒绝
  EXPECT_EQ(ValidatePlatformConfigBinding(
                root_dir.c_str(), "C:\\pipeline.conf",
                static_cast<int32_t>(ALG_BIZ_TYPE_KEYWORD_MATCH), err_buf,
                sizeof(err_buf)),
            -2);

  // 3. UNC 路径拒绝
  EXPECT_EQ(ValidatePlatformConfigBinding(
                root_dir.c_str(), "\\\\server\\share\\pipeline.conf",
                static_cast<int32_t>(ALG_BIZ_TYPE_KEYWORD_MATCH), err_buf,
                sizeof(err_buf)),
            -2);

  // 4. .. 逃逸拒绝
  EXPECT_EQ(ValidatePlatformConfigBinding(
                root_dir.c_str(), "../../etc/passwd",
                static_cast<int32_t>(ALG_BIZ_TYPE_KEYWORD_MATCH), err_buf,
                sizeof(err_buf)),
            -2);
}

// 25. 深度上限与总内存预算超限防御 (R9-005)
TEST_F(PlatformOperatorTest, DepthLimitAndTotalMemoryBudget) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match.conf";
  param.device_id = 0;
  param.platform_type = ChipType::kAx650;

  // 1. 超过最大深度 1024
  param.max_frame_depth = 2048;
  void* handle = nullptr;
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  EXPECT_EQ(handle, nullptr);
}

// 26. 两阶段发布控制块在第 1 个/中间/最后 1 个块失败时的零泄漏与原子回退
// (R9-002, R9-007)
TEST_F(PlatformOperatorTest, AtomicPublishFailureFirstMiddleLast) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match.conf";
  param.device_id = 0;
  param.platform_type = ChipType::kAx650;
  param.max_frame_depth = 10;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  char s1[] = "one", s2[] = "two", s3[] = "three";
  CompanyString cs1{3, s1}, cs2{3, s2}, cs3{5, s3};
  CompanyPlatformKeywordInput in1{1, &cs1}, in2{2, &cs2}, in3{3, &cs3};

  NamedIoBatch batch_inputs(3);
  batch_inputs[0]["client.keyword_in"] = MakeBorrowedPlatformInput(&in1);
  batch_inputs[1]["client.keyword_in"] = MakeBorrowedPlatformInput(&in2);
  batch_inputs[2]["client.keyword_in"] = MakeBorrowedPlatformInput(&in3);

  // 1. 在第 1 个控制块 (countdown=0) 时模拟 bad_alloc
  {
    NamedIoBatch batch_outputs(3);
    batch_outputs[0]["client.keyword_out"] = nullptr;
    batch_outputs[1]["client.keyword_out"] = nullptr;
    batch_outputs[2]["client.keyword_out"] = nullptr;

    alg_framework::OutputPoolState::SetPublishFailureCountdown(0);
    int ret = ops_.Process(handle, batch_inputs, batch_outputs);
    EXPECT_EQ(ret, -99);
    EXPECT_EQ(batch_outputs[0]["client.keyword_out"], nullptr);
    EXPECT_EQ(batch_outputs[1]["client.keyword_out"], nullptr);
    EXPECT_EQ(batch_outputs[2]["client.keyword_out"], nullptr);
  }

  // 2. 在中间第 2 个控制块 (countdown=1) 时模拟 bad_alloc
  {
    NamedIoBatch batch_outputs(3);
    batch_outputs[0]["client.keyword_out"] = nullptr;
    batch_outputs[1]["client.keyword_out"] = nullptr;
    batch_outputs[2]["client.keyword_out"] = nullptr;

    alg_framework::OutputPoolState::SetPublishFailureCountdown(1);
    int ret = ops_.Process(handle, batch_inputs, batch_outputs);
    EXPECT_EQ(ret, -99);
    EXPECT_EQ(batch_outputs[0]["client.keyword_out"], nullptr);
    EXPECT_EQ(batch_outputs[1]["client.keyword_out"], nullptr);
    EXPECT_EQ(batch_outputs[2]["client.keyword_out"], nullptr);
  }

  // 3. 在最后第 3 个控制块 (countdown=2) 时模拟 bad_alloc
  {
    NamedIoBatch batch_outputs(3);
    batch_outputs[0]["client.keyword_out"] = nullptr;
    batch_outputs[1]["client.keyword_out"] = nullptr;
    batch_outputs[2]["client.keyword_out"] = nullptr;

    alg_framework::OutputPoolState::SetPublishFailureCountdown(2);
    int ret = ops_.Process(handle, batch_inputs, batch_outputs);
    EXPECT_EQ(ret, -99);
    EXPECT_EQ(batch_outputs[0]["client.keyword_out"], nullptr);
    EXPECT_EQ(batch_outputs[1]["client.keyword_out"], nullptr);
    EXPECT_EQ(batch_outputs[2]["client.keyword_out"], nullptr);
  }

  // 4. 重置探针后正常执行全部 3 帧
  {
    alg_framework::OutputPoolState::SetPublishFailureCountdown(-1);
    NamedIoBatch batch_outputs(3);
    batch_outputs[0]["client.keyword_out"] = nullptr;
    batch_outputs[1]["client.keyword_out"] = nullptr;
    batch_outputs[2]["client.keyword_out"] = nullptr;

    int ret = ops_.Process(handle, batch_inputs, batch_outputs);
    EXPECT_EQ(ret, 0);
    EXPECT_NE(batch_outputs[0]["client.keyword_out"], nullptr);
    EXPECT_NE(batch_outputs[1]["client.keyword_out"], nullptr);
    EXPECT_NE(batch_outputs[2]["client.keyword_out"], nullptr);
  }

  ops_.Destroy(handle);
}

// 27. 全部 7 类核心业务最大 Batch 边界与两端样本读取正确性验证 (R9-001)
TEST_F(PlatformOperatorTest, MultiBusinessMaxBatchBoundarySuite) {
  std::string root_dir = GetConfDir();

  // 1. DocQA 最大 Batch 压测
  {
    CreateParam param{};
    param.model_path = root_dir.c_str();
    param.cfg_file_name = "configs/pipeline_doc_qa.conf";
    param.device_id = 0;
    param.platform_type = ChipType::kAx650;
    param.max_frame_depth = 8;

    void* handle = nullptr;
    ASSERT_EQ(ops_.Create(&handle, &param), 0);

    constexpr size_t kBatch = 4;
    char q_text[] = "Doc QA Question";
    char d_text[] = "Doc QA Context Document Text";
    CompanyString q_cs{static_cast<int32_t>(std::strlen(q_text)), q_text};
    CompanyString d_cs{static_cast<int32_t>(std::strlen(d_text)), d_text};

    std::vector<CompanyPlatformDocInput> inputs(kBatch);
    NamedIoBatch batch_in(kBatch), batch_out(kBatch);
    for (size_t i = 0; i < kBatch; ++i) {
      inputs[i].request_id = static_cast<uint64_t>(100 + i);
      inputs[i].query_text = &q_cs;
      inputs[i].doc_text = &d_cs;
      batch_in[i]["qa.doc_in"] = MakeBorrowedPlatformInput(&inputs[i]);
      batch_out[i]["qa.doc_out"] = nullptr;
    }

    int ret = ops_.Process(handle, batch_in, batch_out);
    EXPECT_EQ(ret, 0);

    auto* first_out = static_cast<CompanyPlatformDocOutput*>(
        batch_out[0]["qa.doc_out"].get());
    auto* last_out = static_cast<CompanyPlatformDocOutput*>(
        batch_out[kBatch - 1]["qa.doc_out"].get());
    ASSERT_NE(first_out, nullptr);
    ASSERT_NE(last_out, nullptr);
    EXPECT_EQ(first_out->request_id, 100u);
    EXPECT_EQ(last_out->request_id, 100u + kBatch - 1);
    EXPECT_NE(first_out->intent_name, nullptr);
    EXPECT_NE(first_out->answer_text, nullptr);

    ops_.Destroy(handle);
  }

  // 2. DialogueAudit 最大 Batch 压测
  {
    CreateParam param{};
    param.model_path = root_dir.c_str();
    param.cfg_file_name = "configs/pipeline_dialogue_audit.conf";
    param.device_id = 0;
    param.platform_type = ChipType::kAx650;
    param.max_frame_depth = 8;

    void* handle = nullptr;
    ASSERT_EQ(ops_.Create(&handle, &param), 0);

    constexpr size_t kBatch = 4;
    char u_text[] = "Audit Dialogue User Content";
    char c_text[] = "channel_01";
    CompanyString u_cs{static_cast<int32_t>(std::strlen(u_text)), u_text};
    CompanyString c_cs{static_cast<int32_t>(std::strlen(c_text)), c_text};

    std::vector<CompanyPlatformAuditInput> inputs(kBatch);
    NamedIoBatch batch_in(kBatch), batch_out(kBatch);
    for (size_t i = 0; i < kBatch; ++i) {
      inputs[i].request_id = static_cast<uint64_t>(200 + i);
      inputs[i].user_text = &u_cs;
      inputs[i].channel_name = &c_cs;
      batch_in[i]["audit.audit_in"] = MakeBorrowedPlatformInput(&inputs[i]);
      batch_out[i]["audit.audit_out"] = nullptr;
    }

    int ret = ops_.Process(handle, batch_in, batch_out);
    EXPECT_EQ(ret, 0);

    auto* first_out = static_cast<CompanyPlatformAuditOutput*>(
        batch_out[0]["audit.audit_out"].get());
    auto* last_out = static_cast<CompanyPlatformAuditOutput*>(
        batch_out[kBatch - 1]["audit.audit_out"].get());
    ASSERT_NE(first_out, nullptr);
    ASSERT_NE(last_out, nullptr);
    EXPECT_EQ(first_out->request_id, 200u);
    EXPECT_EQ(last_out->request_id, 200u + kBatch - 1);
    EXPECT_NE(first_out->risk_level, nullptr);
    EXPECT_NE(first_out->audit_verdict_json, nullptr);

    ops_.Destroy(handle);
  }

  // 3. AudioAsrIntent 边界与 Batch 压测
  {
    CreateParam param{};
    param.model_path = root_dir.c_str();
    param.cfg_file_name = "configs/pipeline_audio_asr_intent.conf";
    param.device_id = 0;
    param.platform_type = ChipType::kAx650;
    param.max_frame_depth = 8;

    void* handle = nullptr;
    ASSERT_EQ(ops_.Create(&handle, &param), 0);

    constexpr size_t kBatch = 2;
    float pcm_samples[16000] = {0.0f};
    std::vector<CompanyPlatformAudioInput> inputs(kBatch);
    NamedIoBatch batch_in(kBatch), batch_out(kBatch);
    for (size_t i = 0; i < kBatch; ++i) {
      inputs[i].request_id = static_cast<uint64_t>(300 + i);
      inputs[i].sample_rate = 16000;
      inputs[i].pcm_length = 16000;
      inputs[i].pcm_buffer = pcm_samples;
      batch_in[i]["audio.audio_in"] = MakeBorrowedPlatformInput(&inputs[i]);
      batch_out[i]["audio.audio_out"] = nullptr;
    }

    int ret = ops_.Process(handle, batch_in, batch_out);
    EXPECT_EQ(ret, 0);

    auto* first_out = static_cast<CompanyPlatformAudioOutput*>(
        batch_out[0]["audio.audio_out"].get());
    auto* last_out = static_cast<CompanyPlatformAudioOutput*>(
        batch_out[kBatch - 1]["audio.audio_out"].get());
    ASSERT_NE(first_out, nullptr);
    ASSERT_NE(last_out, nullptr);
    EXPECT_EQ(first_out->request_id, 300u);
    EXPECT_EQ(last_out->request_id, 300u + kBatch - 1);
    EXPECT_NE(first_out->transcribed_text, nullptr);
    EXPECT_NE(first_out->intent_slot_json, nullptr);

    ops_.Destroy(handle);
  }

  // 4. CrossRerank 最大 Batch 压测
  {
    CreateParam param{};
    param.model_path = root_dir.c_str();
    param.cfg_file_name = "configs/pipeline_cross_rerank.conf";
    param.device_id = 0;
    param.platform_type = ChipType::kAx650;
    param.max_frame_depth = 8;

    void* handle = nullptr;
    ASSERT_EQ(ops_.Create(&handle, &param), 0);

    constexpr size_t kBatch = 4;
    char q_text[] = "Cross Rerank Query";
    char c_text[] = "Candidate passage content";
    CompanyString q_cs{static_cast<int32_t>(std::strlen(q_text)), q_text};
    CompanyString c_cs{static_cast<int32_t>(std::strlen(c_text)), c_text};

    std::vector<CompanyPlatformRerankInput> inputs(kBatch);
    NamedIoBatch batch_in(kBatch), batch_out(kBatch);
    for (size_t i = 0; i < kBatch; ++i) {
      inputs[i].request_id = static_cast<uint64_t>(400 + i);
      inputs[i].query_text = &q_cs;
      inputs[i].candidate_count = 2;
      inputs[i].candidate_passages[0] = &c_cs;
      inputs[i].candidate_passages[1] = &c_cs;
      batch_in[i]["rank.rerank_in"] = MakeBorrowedPlatformInput(&inputs[i]);
      batch_out[i]["rank.rerank_out"] = nullptr;
    }

    int ret = ops_.Process(handle, batch_in, batch_out);
    EXPECT_EQ(ret, 0);

    auto* first_out = static_cast<CompanyPlatformRerankOutput*>(
        batch_out[0]["rank.rerank_out"].get());
    auto* last_out = static_cast<CompanyPlatformRerankOutput*>(
        batch_out[kBatch - 1]["rank.rerank_out"].get());
    ASSERT_NE(first_out, nullptr);
    ASSERT_NE(last_out, nullptr);
    EXPECT_EQ(first_out->request_id, 400u);
    EXPECT_EQ(last_out->request_id, 400u + kBatch - 1);
    EXPECT_EQ(first_out->count, 2);

    ops_.Destroy(handle);
  }

  // 5. OcrDocQA 最大 Batch 压测
  {
    CreateParam param{};
    param.model_path = root_dir.c_str();
    param.cfg_file_name = "configs/pipeline_ocr_doc_qa.conf";
    param.device_id = 0;
    param.platform_type = ChipType::kAx650;
    param.max_frame_depth = 8;

    void* handle = nullptr;
    ASSERT_EQ(ops_.Create(&handle, &param), 0);

    constexpr size_t kBatch = 2;
    char uri[] = "/tmp/image_test.jpg";
    CompanyString uri_cs{static_cast<int32_t>(std::strlen(uri)), uri};
    char q_text[] = "What is the invoice number?";
    CompanyString q_cs{static_cast<int32_t>(std::strlen(q_text)), q_text};

    std::vector<CompanyFrame> frames(kBatch);
    NamedIoBatch batch_in(kBatch), batch_out(kBatch);
    for (size_t i = 0; i < kBatch; ++i) {
      frames[i].request_id = static_cast<uint64_t>(600 + i);
      frames[i].image_uri = &uri_cs;
      frames[i].metadata = nullptr;
      batch_in[i]["camera_0.frame"] = MakeBorrowedPlatformInput(&frames[i]);
      batch_in[i]["query_channel.string"] = MakeBorrowedPlatformInput(&q_cs);
      batch_out[i]["ocr_result.od_out"] = nullptr;
    }

    int ret = ops_.Process(handle, batch_in, batch_out);
    EXPECT_EQ(ret, 0);

    auto* first_out =
        static_cast<CompanyOdOutput*>(batch_out[0]["ocr_result.od_out"].get());
    ASSERT_NE(first_out, nullptr);
    EXPECT_NE(first_out->result_json, nullptr);

    ops_.Destroy(handle);
  }

  // 6. EntityExtract 最大 Batch 压测
  {
    CreateParam param{};
    param.model_path = root_dir.c_str();
    param.cfg_file_name = "configs/pipeline_entity_extract.conf";
    param.device_id = 0;
    param.platform_type = ChipType::kAx650;
    param.max_frame_depth = 8;

    void* handle = nullptr;
    ASSERT_EQ(ops_.Create(&handle, &param), 0);

    constexpr size_t kBatch = 4;
    char s_text[] = "Alice works at Acme Corp in New York.";
    CompanyString s_cs{static_cast<int32_t>(std::strlen(s_text)), s_text};

    std::vector<CompanyPlatformEntityInput> inputs(kBatch);
    NamedIoBatch batch_in(kBatch), batch_out(kBatch);
    for (size_t i = 0; i < kBatch; ++i) {
      inputs[i].request_id = static_cast<uint64_t>(500 + i);
      inputs[i].sentence_text = &s_cs;
      batch_in[i]["ner.entity_in"] = MakeBorrowedPlatformInput(&inputs[i]);
      batch_out[i]["ner.entity_out"] = nullptr;
    }

    int ret = ops_.Process(handle, batch_in, batch_out);
    EXPECT_EQ(ret, 0);

    auto* first_out = static_cast<CompanyPlatformEntityOutput*>(
        batch_out[0]["ner.entity_out"].get());
    auto* last_out = static_cast<CompanyPlatformEntityOutput*>(
        batch_out[kBatch - 1]["ner.entity_out"].get());
    ASSERT_NE(first_out, nullptr);
    ASSERT_NE(last_out, nullptr);
    EXPECT_EQ(first_out->request_id, 500u);
    EXPECT_EQ(last_out->request_id, 500u + kBatch - 1);
    EXPECT_NE(first_out->entities_json, nullptr);

    ops_.Destroy(handle);
  }
}

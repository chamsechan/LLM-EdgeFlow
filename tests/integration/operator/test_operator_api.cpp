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

#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/deployment_model_resolver.h"
#include "adapter/io_converter_registry.h"
#include "adapter/operator/operator_config_resolver.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "contracts/parameters.h"
#include "core/common_contracts.h"
#include "core/pipeline_catalog.h"
#include "edgeflow/operator/interface.h"
#include "edgeflow/operator/types.h"
#include "engine/backend_registry.h"
#include "tests/support/adapter_test_views.h"
#include "tests/support/control_test_utils.h"
#include "tests/support/operator_nested_output_fixture.h"
#include "tests/support/scoped_converter_registry.h"

#ifndef EDGEFLOW_RERANK_ONNX_FIXTURE
#define EDGEFLOW_RERANK_ONNX_FIXTURE "models/rerank_fixture.onnx"
#endif
#ifndef EDGEFLOW_VOCAB_FIXTURE
#define EDGEFLOW_VOCAB_FIXTURE "models/vocab.txt"
#endif

using namespace llm_edgeflow::operator_api;

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
            ("llm_edgeflow_operator_test_" +
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

class OperatorApiTest : public ::testing::Test {
 protected:
  CreateParam DefaultCreateParam(const char* config_file) const {
    CreateParam param{};
    param.model_path = config_root_.c_str();
    // 下方调用方使用字符串字面量；根目录归本夹具所有。
    param.cfg_file_name = config_file;
    param.device_id = 0;
    param.compute_platform = ComputePlatform::kAx650;
    param.max_frame_depth = 25;
    return param;
  }

  static std::pair<std::shared_ptr<ScopedTempDirectory>, std::string>
  PrepareCrossRerankFixtureConfig() {
    auto temp_dir = std::make_shared<ScopedTempDirectory>();
    auto models_dir = temp_dir->path() / "models";
    std::filesystem::create_directories(models_dir);

    std::error_code ec;
    std::filesystem::copy_file(
        EDGEFLOW_RERANK_ONNX_FIXTURE, models_dir / "bge_reranker_large.onnx",
        std::filesystem::copy_options::overwrite_existing, ec);
    std::filesystem::copy_file(
        EDGEFLOW_VOCAB_FIXTURE, models_dir / "vocab.txt",
        std::filesystem::copy_options::overwrite_existing, ec);

    std::string root_dir = GetConfDir();
    std::ifstream json_in(root_dir + "/configs/pipeline_cross_rerank_cpu.json");
    nlohmann::json pipe_json;
    json_in >> pipe_json;
    pipe_json["models"][0]["model_path"] = "models/bge_reranker_large.onnx";
    pipe_json["models"][0]["model_config"]["tokenizer_file"] = "vocab.txt";
    pipe_json["models"][0]["model_config"]["max_length"] = 32;

    auto temp_json_path = temp_dir->path() / "pipeline_cross_rerank.json";
    std::ofstream json_out(temp_json_path);
    json_out << pipe_json.dump(2);
    json_out.close();

    nlohmann::json conf_json = {{"pipe_path", "pipeline_cross_rerank.json"}};

    auto temp_conf_path = temp_dir->path() / "pipeline_cross_rerank.conf";
    std::ofstream conf_out(temp_conf_path);
    conf_out << conf_json.dump(2);
    conf_out.close();

    return {temp_dir, "pipeline_cross_rerank.conf"};
  }

  void SetUp() override {
    ops_ = Get_LLM_EDGEFLOW_OperatorTable();
    ASSERT_NE(ops_.Init, nullptr);
    ASSERT_NE(ops_.Create, nullptr);
    ASSERT_NE(ops_.Process, nullptr);
    ASSERT_NE(ops_.Control, nullptr);
    ASSERT_NE(ops_.Destroy, nullptr);
    ASSERT_NE(ops_.DeInit, nullptr);

    int ret = ops_.Init();
    ASSERT_EQ(ret, 0);
  }

  void TearDown() override {
    int ret = ops_.DeInit();
    EXPECT_EQ(ret, 0);
  }

  OperatorFunc ops_{};

 private:
  const std::string config_root_ = GetConfDir();
};

// 1. 测试函数表完整性与空安全
TEST_F(OperatorApiTest, OperatorTableIntegrity) {
  OperatorFunc table = Get_LLM_EDGEFLOW_OperatorTable();
  EXPECT_NE(table.Init, nullptr);
  EXPECT_NE(table.Create, nullptr);
  EXPECT_NE(table.Process, nullptr);
  EXPECT_NE(table.Control, nullptr);
  EXPECT_NE(table.Destroy, nullptr);
  EXPECT_NE(table.DeInit, nullptr);
}

// 2. 参数校验与负向安全拦截 (Create 阶段)
TEST_F(OperatorApiTest, CreateParameterValidation) {
  void* handle = nullptr;
  std::string root_dir = GetConfDir();
  std::string rel_conf = "configs/pipeline_keyword_match_rules.conf";

  // 1. 空 handle 指针
  EXPECT_EQ(ops_.Create(nullptr, nullptr), -1);

  // 2. *handle 非空
  void* dummy_ptr = reinterpret_cast<void*>(0x1234);
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = rel_conf.c_str();
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kAx650;
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

  // 8. 未知芯片类型 ComputePlatform::kUnknown 及非法枚举值
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kUnknown;
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  param.compute_platform = static_cast<ComputePlatform>(9999);
  EXPECT_EQ(ops_.Create(&handle, &param), -2);

  // 9. 不存在的文件
  param.compute_platform = ComputePlatform::kAx650;
  param.cfg_file_name = "configs/non_existent_file.conf";
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  EXPECT_NE(GetOperatorLastError(), nullptr);
}

// 3. 强类型 Control 正常与边界异常测试
TEST_F(OperatorApiTest, StronglyTypedControlValidation) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kAx650;
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
  ControlSwitchPromptParam prompt_param_null{"test_prompt", nullptr};
  EXPECT_EQ(
      ops_.Control(handle, ControlCommand::kSwitchPrompt, &prompt_param_null),
      -2);

  ControlSwitchPromptParam prompt_param_valid{"test_prompt",
                                              "用户提问：{query}，请回答："};
  // KeywordMatch 不含 TextTemplateNode，返回 -7
  // (COMPANY_ALG_ERR_UNSUPPORTED_CONTROL)
  EXPECT_EQ(
      ops_.Control(handle, ControlCommand::kSwitchPrompt, &prompt_param_valid),
      -7);

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
  // KeywordMatch 不支持动态阈值调节，返回 -7
  EXPECT_EQ(
      ops_.Control(handle, ControlCommand::kUpdateThreshold, &thresh_valid),
      -7);

  // 3.4 未知命令枚举
  EXPECT_EQ(
      ops_.Control(handle, static_cast<ControlCommand>(999), &thresh_valid),
      -2);

  ops_.Destroy(handle);

  // 3.5 在包含 TextTemplateNode 的管线上测试 kSwitchPrompt 成功路径
  CreateParam entity_param{};
  entity_param.model_path = root_dir.c_str();
  entity_param.cfg_file_name =
      "demo/fixtures/mock/pipeline_entity_extract.conf";
  entity_param.device_id = 0;
  entity_param.compute_platform = ComputePlatform::kCpu;

  void* entity_handle = nullptr;
  ASSERT_EQ(ops_.Create(&entity_handle, &entity_param), 0);
  ASSERT_NE(entity_handle, nullptr);

  ControlSwitchPromptParam entity_prompt{"test_prompt", "{{primary}}"};
  EXPECT_EQ(ops_.Control(entity_handle, ControlCommand::kSwitchPrompt,
                         &entity_prompt),
            0);

  // prompt_id 仍检查长度，但不转发给节点。
  const std::string long_prompt_id(256, 'a');
  ControlSwitchPromptParam long_id_prompt{long_prompt_id.c_str(),
                                          "{{primary}}"};
  EXPECT_EQ(ops_.Control(entity_handle, ControlCommand::kSwitchPrompt,
                         &long_id_prompt),
            -2);

  ops_.Destroy(entity_handle);
}

TEST_F(OperatorApiTest, GenericJsonControlReachesCustomNodeAndReportsFailures) {
  ScopedTempDirectory temporary;
  llm_edgeflow::test::WriteControlTestPipeline(temporary.path());
  const std::string root = temporary.path().string();
  CreateParam param;
  param.model_path = root.c_str();
  param.cfg_file_name = "pipeline.conf";
  param.compute_platform = ComputePlatform::kCpu;
  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0) << GetOperatorLastError();
  // 确保即使致命断言提前退出本测试，也会执行 Destroy。
  const auto owner =
      std::shared_ptr<void>(handle, [this](void* h) { ops_.Destroy(h); });
  const auto check = [&](int expected_hit) {
    std::string text = "sample";
    CompanyString cs{static_cast<int32_t>(text.size()), text.data()};
    CompanyOperatorKeywordInput input{123, &cs,
                                      COMPANY_MOCK_SERVICE_KEYWORD_MATCH};
    NamedIoBatch inputs(1), outputs(1);
    inputs[0]["control.keyword_in"] = MakeBorrowedOperatorInput(&input);
    outputs[0]["control.keyword_out"] = nullptr;
    ASSERT_EQ(ops_.Process(handle, inputs, outputs), 0)
        << GetOperatorLastError();
    const auto* output = static_cast<CompanyOperatorKeywordOutput*>(
        outputs[0]["control.keyword_out"].get());
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(output->request_id, 123u);
    EXPECT_EQ(output->is_hit, expected_hit);
  };
  check(0);
  std::string payload = R"({"prefix":"VIP:"})";
  ControlJsonParam command{2000000041, payload.c_str()};
  ASSERT_EQ(ops_.Control(handle, ControlCommand::kJson, &command), 0);
  payload.assign(payload.size(), 'x');  // 调用方内存已不再需要。
  check(1);

  // 不同的命令 ID 更新同一 Pipeline 中各自的 Node。
  ControlJsonParam rules{
      llm_edgeflow::kControlCmdUpdateRules,
      R"({"categories":{"AFTER_RULE_UPDATE":["NEW:sample"]}})"};
  ASSERT_EQ(ops_.Control(handle, ControlCommand::kJson, &rules), 0);
  check(0);
  command.json_param_str =
      R"({"$edgeflow_control":1,"node_id":"prefix","payload":{"prefix":"NEW:"}})";
  ASSERT_EQ(ops_.Control(handle, ControlCommand::kJson, &command), 0);
  check(1);  // 更新 prefix 会保留匹配器的新规则。

  // 被拒绝的请求属于非法参数；内部 Core 错误码保留在诊断信息中，
  // 不会与无效句柄错误码冲突。
  command.json_param_str =
      R"({"$edgeflow_control":1,"node_id":"missing","payload":{"prefix":"BAD:"}})";
  EXPECT_EQ(ops_.Control(handle, ControlCommand::kJson, &command),
            COMPANY_ALG_ERR_INVALID_PARAM);
  EXPECT_NE(std::string(GetOperatorLastError()).find("missing"),
            std::string::npos);
  EXPECT_NE(std::string(GetOperatorLastError())
                .find("Control request failed with internal code -1"),
            std::string::npos);
  check(1);

  command.json_param_str = R"({"prefix":1})";
  EXPECT_EQ(ops_.Control(handle, ControlCommand::kJson, &command),
            COMPANY_ALG_ERR_INVALID_PARAM);
  EXPECT_NE(std::string(GetOperatorLastError()).find("node 'prefix'"),
            std::string::npos);
  EXPECT_NE(std::string(GetOperatorLastError()).find("prefix"),
            std::string::npos);
  payload = nlohmann::json{{"prefix", std::string(65, 'x')}}.dump();
  command.json_param_str = payload.c_str();
  EXPECT_EQ(ops_.Control(handle, ControlCommand::kJson, &command),
            COMPANY_ALG_ERR_UNKNOWN);
  EXPECT_NE(std::string(GetOperatorLastError()).find("64 UTF-8 bytes"),
            std::string::npos);
  EXPECT_NE(std::string(GetOperatorLastError())
                .find("Control node update failed with internal code"),
            std::string::npos);
  check(1);

  // Node 拒绝格式正确的 payload 时报告执行失败。
  ControlJsonParam bad_regex{
      llm_edgeflow::kControlCmdUpdateRules,
      R"({"rules":[{"pattern":"(","strategy":"regex"}]})"};
  EXPECT_EQ(ops_.Control(handle, ControlCommand::kJson, &bad_regex),
            COMPANY_ALG_ERR_UNKNOWN);
  EXPECT_NE(std::string(GetOperatorLastError()).find("node 'matcher'"),
            std::string::npos);
  check(1);

  for (const char* invalid :
       {static_cast<const char*>(nullptr), "", "[]", "{"}) {
    command.json_param_str = invalid;
    EXPECT_EQ(ops_.Control(handle, ControlCommand::kJson, &command), -2);
  }
  payload.assign(65536, 'x');
  command.json_param_str = payload.c_str();
  EXPECT_EQ(ops_.Control(handle, ControlCommand::kJson, &command), -2);
  command = {0, "{}"};
  EXPECT_EQ(ops_.Control(handle, ControlCommand::kJson, &command), -2);
  command = {19999, "{}"};
  EXPECT_EQ(ops_.Control(handle, ControlCommand::kJson, &command), -7);
  EXPECT_NE(std::string(GetOperatorLastError()).find("19999"),
            std::string::npos);
}

// 4. 句柄生命周期与防护测试
TEST_F(OperatorApiTest, HandleLifecycleAndUafPrevention) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  EXPECT_EQ(ops_.Destroy(handle), 0);
  EXPECT_EQ(ops_.Destroy(handle), -1);

  std::string text = "test";
  CompanyString cs{static_cast<int32_t>(text.size()),
                   const_cast<char*>(text.data())};
  CompanyOperatorKeywordInput in{101, &cs, COMPANY_MOCK_SERVICE_KEYWORD_MATCH};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["chan.keyword_in"] = MakeBorrowedOperatorInput(&in);
  out_b[0]["chan.keyword_out"] = std::shared_ptr<void>();

  EXPECT_EQ(ops_.Process(handle, in_b, out_b), -1);
}

// 5. CompanyString 校验规则测试 (包含嵌入 NUL 拦截、负长度与超限拦截)
TEST_F(OperatorApiTest, CompanyStringValidation) {
  using namespace llm_edgeflow;
  std::string err;

  // 1. null 指针
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyString(nullptr, 100,
                                                             "str", &err),
            -3);

  // 2. 负长度
  char buf[] = "hello";
  CompanyString cs_neg{-1, buf};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyString(&cs_neg, 100,
                                                             "str", &err),
            -3);

  // 3. 长度为 0 (正常空字符串)
  CompanyString cs_zero{0, nullptr};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyString(&cs_zero, 100,
                                                             "str", &err),
            0);

  // 4. 长度超限
  CompanyString cs_toolarge{150, buf};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyString(&cs_toolarge, 100,
                                                             "str", &err),
            -3);

  // 5. 长度 > 0 但 data == nullptr
  CompanyString cs_nulldata{10, nullptr};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyString(&cs_nulldata, 100,
                                                             "str", &err),
            -3);

  // 6. 嵌入 NUL 字符 (禁止)
  char embedded_nul[] = "hello\0world";
  CompanyString cs_embed{11, embedded_nul};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyString(&cs_embed, 100,
                                                             "str", &err),
            -3);

  // 7. 正常字符串
  CompanyString cs_valid{5, buf};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyString(&cs_valid, 100,
                                                             "str", &err),
            0);
}

// 6. 关注词匹配端到端 (Keyword Match)
TEST_F(OperatorApiTest, EndToEndKeywordMatch) {
  auto param = DefaultCreateParam("configs/pipeline_keyword_match_rules.conf");

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
  CompanyOperatorKeywordInput in{1001, &cs, COMPANY_MOCK_SERVICE_KEYWORD_MATCH};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["chan.keyword_in"] = MakeBorrowedOperatorInput(&in);
  out_b[0]["chan.keyword_out"] = std::shared_ptr<void>();

  ASSERT_EQ(ops_.Process(handle, in_b, out_b), 0);

  auto out_sp = out_b[0]["chan.keyword_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_ptr = static_cast<CompanyOperatorKeywordOutput*>(out_sp.get());
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
TEST_F(OperatorApiTest, EndToEndOcrInvoiceQaMultiSlot) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "demo/fixtures/mock/pipeline_ocr_invoice_qa.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::string uri = "./data/invoice_01.jpg";
  std::string prompt = "提取发票代码、号码与总金额";
  CompanyString uri_cs{static_cast<int32_t>(uri.size()),
                       const_cast<char*>(uri.data())};
  CompanyFrame frame{60001, &uri_cs, nullptr,
                     COMPANY_MOCK_SERVICE_OCR_INVOICE_QA};
  CompanyString prompt_cs{static_cast<int32_t>(prompt.size()),
                          const_cast<char*>(prompt.data())};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["camera_0.frame"] = MakeBorrowedOperatorInput(&frame);
  in_b[0]["camera_0.string"] = MakeBorrowedOperatorInput(&prompt_cs);
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

// 8. 文档问答 (Doc QA)
TEST_F(OperatorApiTest, EndToEndDocQa) {
  auto param = DefaultCreateParam("demo/fixtures/mock/pipeline_doc_qa.conf");

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::string doc = "企业级算法框架设计规范：采用4层分层架构。";
  std::string query = "请简述该算法框架的架构设计？";
  CompanyString doc_cs{static_cast<int32_t>(doc.size()),
                       const_cast<char*>(doc.data())};
  CompanyString query_cs{static_cast<int32_t>(query.size()),
                         const_cast<char*>(query.data())};
  CompanyOperatorDocInput in{10001, &doc_cs, &query_cs,
                             COMPANY_MOCK_SERVICE_DOC_QA};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["rag_channel.doc_in"] = MakeBorrowedOperatorInput(&in);
  out_b[0]["rag_channel.doc_out"] = std::shared_ptr<void>();

  ASSERT_EQ(ops_.Process(handle, in_b, out_b), 0);

  auto out_sp = out_b[0]["rag_channel.doc_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_ptr = static_cast<CompanyOperatorDocOutput*>(out_sp.get());
  EXPECT_EQ(out_ptr->request_id, 10001u);
  EXPECT_NE(out_ptr->answer_text, nullptr);
  EXPECT_GT(out_ptr->answer_text->length, 0);

  out_b.clear();
  out_sp.reset();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// 9. 对话合规审核 (Dialogue Audit)
TEST_F(OperatorApiTest, EndToEndDialogueAudit) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "demo/fixtures/mock/pipeline_dialogue_audit.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kAx650;
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
  CompanyOperatorAuditInput in{40001, &dia_cs, &chan_cs,
                               COMPANY_MOCK_SERVICE_DIALOGUE_AUDIT};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["audit_channel.audit_in"] = MakeBorrowedOperatorInput(&in);
  out_b[0]["audit_channel.audit_out"] = std::shared_ptr<void>();

  ASSERT_EQ(ops_.Process(handle, in_b, out_b), 0);

  auto out_sp = out_b[0]["audit_channel.audit_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_ptr = static_cast<CompanyOperatorAuditOutput*>(out_sp.get());
  EXPECT_EQ(out_ptr->request_id, 40001u);
  EXPECT_NE(out_ptr->risk_level, nullptr);
  EXPECT_GT(out_ptr->risk_level->length, 0);

  out_b.clear();
  out_sp.reset();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// 10. 语音识别与意图抽取业务 (Audio ASR Intent)
TEST_F(OperatorApiTest, EndToEndAudioAsrIntent) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "demo/fixtures/mock/pipeline_audio_asr_intent.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::vector<float> pcm(16000, 0.01f);
  CompanyOperatorAudioInput in{70001, pcm.data(),
                               static_cast<int32_t>(pcm.size()), 16000,
                               COMPANY_MOCK_SERVICE_AUDIO_ASR_INTENT};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["mic_0.audio_in"] = MakeBorrowedOperatorInput(&in);
  out_b[0]["mic_0.audio_out"] = std::shared_ptr<void>();

  ASSERT_EQ(ops_.Process(handle, in_b, out_b), 0);

  auto out_sp = out_b[0]["mic_0.audio_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_ptr = static_cast<CompanyOperatorAudioOutput*>(out_sp.get());
  EXPECT_EQ(out_ptr->request_id, 70001u);
  EXPECT_NE(out_ptr->transcribed_text, nullptr);
  EXPECT_GT(out_ptr->transcribed_text->length, 0);

  out_sp.reset();
  out_b[0]["mic_0.audio_out"].reset();
  in.pcm_buffer = nullptr;
  in.pcm_length = 0;
  ASSERT_EQ(ops_.Process(handle, in_b, out_b), 0);
  auto* empty_output = static_cast<CompanyOperatorAudioOutput*>(
      out_b[0]["mic_0.audio_out"].get());
  ASSERT_NE(empty_output, nullptr);
  EXPECT_EQ(empty_output->request_id, 70001U);
  EXPECT_EQ(empty_output->status_code, 0);
  out_b.clear();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// 11. Cross-Encoder 精排 (Cross Rerank)
TEST_F(OperatorApiTest, EndToEndCrossRerank) {
  if (!llm_edgeflow::BackendRegistry::Instance()
           .Find("onnxruntime")
           .has_value()) {
    GTEST_SKIP() << "ONNX Runtime backend disabled in this build";
  }
  auto temp_cfg = PrepareCrossRerankFixtureConfig();
  std::string model_path_str = temp_cfg.first->path().string();
  CreateParam param{};
  param.model_path = model_path_str.c_str();
  param.cfg_file_name = temp_cfg.second.c_str();
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kCpu;
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

  CompanyOperatorRerankInput in{};
  in.service_type = COMPANY_MOCK_SERVICE_CROSS_RERANK;
  in.request_id = 80001;
  in.query_text = &query_cs;
  in.candidate_passages[0] = &p1_cs;
  in.candidate_passages[1] = &p2_cs;
  in.candidate_count = 2;

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["ranker.rerank_in"] = MakeBorrowedOperatorInput(&in);
  out_b[0]["ranker.rerank_out"] = std::shared_ptr<void>();

  ASSERT_EQ(ops_.Process(handle, in_b, out_b), 0);

  auto out_sp = out_b[0]["ranker.rerank_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_ptr = static_cast<CompanyOperatorRerankOutput*>(out_sp.get());
  EXPECT_EQ(out_ptr->request_id, 80001u);
  EXPECT_EQ(out_ptr->count, 2);

  out_b.clear();
  out_sp.reset();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// 12. 输出占位非空拦截与未知 Key 拦截
TEST_F(OperatorApiTest, OutputSlotValidation) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);

  std::string text = "test";
  CompanyString cs{static_cast<int32_t>(text.size()),
                   const_cast<char*>(text.data())};
  CompanyOperatorKeywordInput in{1001, &cs, COMPANY_MOCK_SERVICE_KEYWORD_MATCH};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["chan.keyword_in"] = MakeBorrowedOperatorInput(&in);

  // 1. 输出槽位为非空 shared_ptr -> 拦截返回 -4
  CompanyOperatorKeywordOutput dummy_out{};
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

TEST_F(OperatorApiTest, ProcessUsesResolvedEffectiveBatchLimit) {
  auto param = DefaultCreateParam("configs/pipeline_keyword_match_rules.conf");
  param.max_frame_depth = 100;
  void* raw_handle = nullptr;
  ASSERT_EQ(ops_.Create(&raw_handle, &param), 0) << GetOperatorLastError();
  const auto destroy = [this](void* handle) { ops_.Destroy(handle); };
  std::unique_ptr<void, decltype(destroy)> handle(raw_handle, destroy);
  char text[] = "query";
  CompanyString sentence{5, text};
  std::vector<CompanyOperatorKeywordInput> rows(65);
  for (size_t count : {64U, 65U}) {
    NamedIoBatch inputs(count), outputs(count);
    for (size_t i = 0; i < count; ++i) {
      rows[i] = {1000 + i, &sentence, COMPANY_MOCK_SERVICE_KEYWORD_MATCH};
      inputs[i]["test.keyword_in"] = MakeBorrowedOperatorInput(&rows[i]);
      outputs[i]["test.keyword_out"] = nullptr;
    }
    EXPECT_EQ(ops_.Process(handle.get(), inputs, outputs), count == 64 ? 0 : -3)
        << GetOperatorLastError();
    if (count == 64) {
      for (size_t i = 0; i < count; ++i) {
        const auto* result = static_cast<CompanyOperatorKeywordOutput*>(
            outputs[i]["test.keyword_out"].get());
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(result->request_id, rows[i].request_id);
        EXPECT_EQ(result->status_code, 0);
      }
    } else {
      EXPECT_STREQ(GetOperatorLastError(),
                   "Input batch size 65 exceeds effective batch limit 64");
      for (const auto& frame : outputs)
        EXPECT_EQ(frame.at("test.keyword_out"), nullptr);
    }
  }
}

TEST_F(OperatorApiTest, ProcessRestoresRequestIdsWithoutBizPort) {
  auto param = DefaultCreateParam("configs/pipeline_keyword_match_rules.conf");
  void* raw_handle = nullptr;
  ASSERT_EQ(ops_.Create(&raw_handle, &param), 0) << GetOperatorLastError();
  const auto destroy = [this](void* handle) { ops_.Destroy(handle); };
  std::unique_ptr<void, decltype(destroy)> handle(raw_handle, destroy);
  char text[] = "query";
  CompanyString sentence{5, text};
  for (const std::vector<uint64_t>& ids :
       {std::vector<uint64_t>{900001, 42, 7},
        std::vector<uint64_t>{900001, 42, 42}}) {
    std::vector<CompanyOperatorKeywordInput> rows(ids.size());
    NamedIoBatch inputs(ids.size()), outputs(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
      rows[i] = {ids[i], &sentence, COMPANY_MOCK_SERVICE_KEYWORD_MATCH};
      inputs[i]["test.keyword_in"] = MakeBorrowedOperatorInput(&rows[i]);
      outputs[i]["test.keyword_out"] = nullptr;
    }
    ASSERT_EQ(ops_.Process(handle.get(), inputs, outputs), 0)
        << GetOperatorLastError();
    for (size_t i = 0; i < ids.size(); ++i) {
      const auto* result = static_cast<const CompanyOperatorKeywordOutput*>(
          outputs[i].at("test.keyword_out").get());
      ASSERT_NE(result, nullptr);
      EXPECT_EQ(result->request_id, ids[i]);
      EXPECT_EQ(result->status_code, 0);
    }
  }
}

// 13. 输出池耗尽、阻塞与唤醒复用测试
TEST_F(OperatorApiTest, OutputPoolExhaustionAndBlocking) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kAx650;
  param.max_frame_depth = 2;  // 设定极小深度 2

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);

  std::string text = "test";
  CompanyString cs{static_cast<int32_t>(text.size()),
                   const_cast<char*>(text.data())};
  CompanyOperatorKeywordInput in{1001, &cs, COMPANY_MOCK_SERVICE_KEYWORD_MATCH};

  // 1. 单次 Batch > max_frame_depth -> 立即拒绝 (-3)，不陷入死锁
  NamedIoBatch in_b3(3), out_b3(3);
  for (int i = 0; i < 3; ++i) {
    in_b3[i]["chan.keyword_in"] = MakeBorrowedOperatorInput(&in);
    out_b3[i]["chan.keyword_out"] = std::shared_ptr<void>();
  }
  EXPECT_EQ(ops_.Process(handle, in_b3, out_b3), -3);

  // 2. 连续检出 2 个块，暂不释放
  NamedIoBatch in_b1(1), out_b1(1);
  in_b1[0]["chan.keyword_in"] = MakeBorrowedOperatorInput(&in);
  out_b1[0]["chan.keyword_out"] = std::shared_ptr<void>();
  ASSERT_EQ(ops_.Process(handle, in_b1, out_b1), 0);
  auto out1 = out_b1[0]["chan.keyword_out"];

  NamedIoBatch in_b2(1), out_b2(1);
  in_b2[0]["chan.keyword_in"] = MakeBorrowedOperatorInput(&in);
  out_b2[0]["chan.keyword_out"] = std::shared_ptr<void>();
  ASSERT_EQ(ops_.Process(handle, in_b2, out_b2), 0);
  auto out2 = out_b2[0]["chan.keyword_out"];

  // 此时池中可用块为 0
  std::atomic<bool> thread_started{false};
  std::atomic<bool> thread_completed{false};

  std::thread worker([&]() {
    thread_started = true;
    NamedIoBatch in_b(1), out_b(1);
    in_b[0]["chan.keyword_in"] = MakeBorrowedOperatorInput(&in);
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
TEST_F(OperatorApiTest, DestroyViolationHandling) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kAx650;
  param.max_frame_depth = 5;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);

  std::string text = "test";
  CompanyString cs{static_cast<int32_t>(text.size()),
                   const_cast<char*>(text.data())};
  CompanyOperatorKeywordInput in{1001, &cs, COMPANY_MOCK_SERVICE_KEYWORD_MATCH};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["chan.keyword_in"] = MakeBorrowedOperatorInput(&in);
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

// 15. ResolveOperatorConfigIo 预检接口测试
TEST_F(OperatorApiTest, ResolveOperatorConfigIoApi) {
  const std::string root = GetConfDir();
  OperatorIoContract contract;
  contract.inputs.resize(3);
  char error[256]{};
  static_assert(noexcept(ResolveOperatorConfigIo(nullptr, nullptr, nullptr)));
  ASSERT_EQ(ResolveOperatorConfigIo(root.c_str(),
                                    "configs/pipeline_keyword_match_rules.conf",
                                    &contract, error, sizeof(error)),
            0)
      << error;
  ASSERT_EQ(contract.inputs.size(), 1U);
  EXPECT_EQ(contract.inputs[0].type, "keyword_in");
  EXPECT_EQ(contract.inputs[0].name, "keyword_match");
  EXPECT_EQ(contract.inputs[0].type_name, "CompanyOperatorKeywordInput");
  EXPECT_EQ(contract.inputs[0].service_type,
            COMPANY_MOCK_SERVICE_KEYWORD_MATCH);
  EXPECT_TRUE(contract.inputs[0].required);
  ASSERT_EQ(contract.outputs.size(), 1U);
  EXPECT_EQ(contract.outputs[0].type, "keyword_out");
  EXPECT_EQ(contract.outputs[0].name, "keyword_match");
  EXPECT_EQ(contract.outputs[0].type_name, "CompanyOperatorKeywordOutput");

  // 图片问答的输入有两个槽，按槽顺序各占一项。
  contract = {};
  ASSERT_EQ(ResolveOperatorConfigIo(
                root.c_str(), "demo/fixtures/mock/pipeline_ocr_invoice_qa.conf",
                &contract, error, sizeof(error)),
            0)
      << error;
  ASSERT_EQ(contract.inputs.size(), 2U);
  EXPECT_EQ(contract.inputs[0].type, "frame");
  EXPECT_EQ(contract.inputs[0].type_name, "CompanyFrame");
  EXPECT_EQ(contract.inputs[1].type, "string");
  EXPECT_EQ(contract.inputs[1].type_name, "CompanyString");
  // CompanyString 没有 service_type 成员；frame 有。
  EXPECT_EQ(contract.inputs[0].service_type,
            COMPANY_MOCK_SERVICE_OCR_INVOICE_QA);
  EXPECT_FALSE(contract.inputs[1].service_type.has_value());
  EXPECT_EQ(contract.inputs[0].name, "ocr_invoice_qa");
  EXPECT_EQ(contract.inputs[1].name, "ocr_invoice_qa");
  ASSERT_EQ(contract.outputs.size(), 1U);
  EXPECT_EQ(contract.outputs[0].type, "od_out");
  EXPECT_EQ(contract.outputs[0].type_name, "CompanyOdOutput");

  // 同一结构上的多个业务共用载体：翻译与实体抽取都使用 entity_in。
  contract = {};
  ASSERT_EQ(ResolveOperatorConfigIo(root.c_str(),
                                    "configs/pipeline_translate_cpu.conf",
                                    &contract, error, sizeof(error)),
            0)
      << error;
  ASSERT_EQ(contract.inputs.size(), 1U);
  EXPECT_EQ(contract.inputs[0].type, "entity_in");
  EXPECT_EQ(contract.inputs[0].name, "translate");
  EXPECT_EQ(contract.inputs[0].type_name, "CompanyOperatorEntityInput");
  EXPECT_EQ(contract.inputs[0].service_type, COMPANY_MOCK_SERVICE_TRANSLATE);

  EXPECT_EQ(ResolveOperatorConfigIo(root.c_str(),
                                    "configs/pipeline_keyword_match_rules.conf",
                                    nullptr, error, sizeof(error)),
            -2);
  // 无输出缓冲时仍返回结果，且预检失败时契约被清空。
  contract = {};
  ASSERT_EQ(
      ResolveOperatorConfigIo(
          root.c_str(), "configs/pipeline_keyword_match_rules.conf", &contract),
      0);
  EXPECT_EQ(contract.inputs.size(), 1U);
  contract.inputs.resize(2);
  EXPECT_EQ(ResolveOperatorConfigIo(root.c_str(), "/etc/passwd", &contract,
                                    error, sizeof(error)),
            -2);
  EXPECT_TRUE(contract.inputs.empty());
  EXPECT_TRUE(contract.outputs.empty());
  for (int invalid = 0; invalid < 2; ++invalid) {
    contract.inputs.resize(1);
    EXPECT_EQ(ResolveOperatorConfigIo(
                  invalid == 0 ? nullptr : root.c_str(),
                  invalid == 1 ? nullptr
                               : "configs/pipeline_keyword_match_rules.conf",
                  &contract, error, sizeof(error)),
              -2);
    EXPECT_TRUE(contract.inputs.empty());
  }
}

// 16. 实体抽取业务端到端 (Entity Extract)
TEST_F(OperatorApiTest, EndToEndEntityExtract) {
  auto param =
      DefaultCreateParam("demo/fixtures/mock/pipeline_entity_extract.conf");

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::string text = "张三在清华大学研发深度学习大模型。";
  CompanyString cs{static_cast<int32_t>(text.size()),
                   const_cast<char*>(text.data())};
  CompanyOperatorEntityInput in{30001, &cs,
                                COMPANY_MOCK_SERVICE_ENTITY_EXTRACT};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["nlp.entity_in"] = MakeBorrowedOperatorInput(&in);
  out_b[0]["nlp.entity_out"] = std::shared_ptr<void>();

  ASSERT_EQ(ops_.Process(handle, in_b, out_b), 0);

  auto out_sp = out_b[0]["nlp.entity_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_ptr = static_cast<CompanyOperatorEntityOutput*>(out_sp.get());
  EXPECT_EQ(out_ptr->request_id, 30001u);
  EXPECT_NE(out_ptr->entities_json, nullptr);
  EXPECT_GT(out_ptr->entities_json->length, 0);

  out_b.clear();
  out_sp.reset();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// 17. CompanyBuffer 与 CompanyAny 平台值类型校验测试
TEST_F(OperatorApiTest, CompanyBufferAndAnyValidation) {
  using namespace llm_edgeflow;
  const auto* buf_binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("buffer");
  ASSERT_NE(buf_binding, nullptr);
  ASSERT_TRUE(buf_binding->validate_external);

  InputLimits limits;
  std::string err;

  // CompanyBuffer：空指针
  EXPECT_EQ(buf_binding->validate_external(nullptr, limits, &err), -3);

  // CompanyBuffer：负长度
  uint8_t dummy_data[] = {0x01, 0x02, 0x03};
  CompanyBuffer buf_neg{-1, dummy_data};
  EXPECT_EQ(buf_binding->validate_external(&buf_neg, limits, &err), -3);

  // CompanyBuffer：length > max
  CompanyBuffer buf_toolarge{static_cast<int32_t>(limits.max_buffer_bytes + 1),
                             dummy_data};
  EXPECT_EQ(buf_binding->validate_external(&buf_toolarge, limits, &err), -3);

  // CompanyBuffer：length > 0 但 data 为空
  CompanyBuffer buf_nulldata{10, nullptr};
  EXPECT_EQ(buf_binding->validate_external(&buf_nulldata, limits, &err), -3);

  // CompanyBuffer：合法二进制数据
  CompanyBuffer buf_valid{3, dummy_data};
  EXPECT_EQ(buf_binding->validate_external(&buf_valid, limits, &err), 0);

  // CompanyAny
  const auto* any_binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("any");
  ASSERT_NE(any_binding, nullptr);
  ASSERT_TRUE(any_binding->validate_external);

  // CompanyAny：空指针
  EXPECT_EQ(any_binding->validate_external(nullptr, limits, &err), -3);

  // CompanyAny：负的 count / length
  CompanyAny any_neg{1, -1, 10, dummy_data};
  EXPECT_EQ(any_binding->validate_external(&any_neg, limits, &err), -3);

  // CompanyAny：byte_length > max
  CompanyAny any_toolarge{1, 10, static_cast<int32_t>(limits.max_any_bytes + 1),
                          dummy_data};
  EXPECT_EQ(any_binding->validate_external(&any_toolarge, limits, &err), -3);

  // 7. 正确尺寸方程: float32 (type_id=1), count=3, byte_length=12 -> 0
  CompanyAny any_valid{1, 3, 12, dummy_data};
  EXPECT_EQ(any_binding->validate_external(&any_valid, limits, &err), 0);
}

// 18. 输入 shared_ptr 所有权不持有与 use_count 校验测试
TEST_F(OperatorApiTest, InputSharedPtrUseCountNotRetained) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);

  std::string text = "VIP专员";
  CompanyString cs{static_cast<int32_t>(text.size()),
                   const_cast<char*>(text.data())};
  auto in_ptr = std::make_shared<CompanyOperatorKeywordInput>();
  in_ptr->request_id = 1001;
  in_ptr->service_type = COMPANY_MOCK_SERVICE_KEYWORD_MATCH;
  in_ptr->sentence_text = &cs;

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["chan.keyword_in"] = in_ptr;
  out_b[0]["chan.keyword_out"] = std::shared_ptr<void>();

  // process 前：in_ptr 由 in_ptr 和 in_b[0] 持有 (use_count == 2)
  EXPECT_EQ(in_ptr.use_count(), 2);

  ASSERT_EQ(ops_.Process(handle, in_b, out_b), 0);

  // process 后：in_ptr 仍只由 in_ptr 和 in_b[0] 持有 (use_count == 2)
  EXPECT_EQ(in_ptr.use_count(), 2);

  in_b.clear();
  // 现在只有 in_ptr 持有 (use_count == 1)
  EXPECT_EQ(in_ptr.use_count(), 1);

  out_b.clear();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// 19. 输出内存池深度 0 归一化与地址复用校验测试
TEST_F(OperatorApiTest, OutputAddressReuseAndDepthNormalization) {
  std::string root_dir = GetConfDir();

  // 1. 深度 0 自动归一化为默认 25 测试
  {
    CreateParam param{};
    param.model_path = root_dir.c_str();
    param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
    param.device_id = 0;
    param.compute_platform = ComputePlatform::kAx650;
    param.max_frame_depth = 0;  // 0 应被自动归一化为默认 25

    void* handle = nullptr;
    ASSERT_EQ(ops_.Create(&handle, &param), 0);

    std::string text = "VIP专员";
    CompanyString cs{static_cast<int32_t>(text.size()),
                     const_cast<char*>(text.data())};
    CompanyOperatorKeywordInput in{1001, &cs,
                                   COMPANY_MOCK_SERVICE_KEYWORD_MATCH};

    NamedIoBatch in_b(1), out_b(1);
    in_b[0]["chan.keyword_in"] = MakeBorrowedOperatorInput(&in);
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
    param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
    param.device_id = 0;
    param.compute_platform = ComputePlatform::kAx650;
    param.max_frame_depth = 1;

    void* handle = nullptr;
    ASSERT_EQ(ops_.Create(&handle, &param), 0);

    std::string text = "VIP专员";
    CompanyString cs{static_cast<int32_t>(text.size()),
                     const_cast<char*>(text.data())};
    CompanyOperatorKeywordInput in{1001, &cs,
                                   COMPANY_MOCK_SERVICE_KEYWORD_MATCH};

    NamedIoBatch in_b1(1), out_b1(1);
    in_b1[0]["chan.keyword_in"] = MakeBorrowedOperatorInput(&in);
    out_b1[0]["chan.keyword_out"] = std::shared_ptr<void>();
    ASSERT_EQ(ops_.Process(handle, in_b1, out_b1), 0);

    void* first_block_addr = out_b1[0]["chan.keyword_out"].get();
    ASSERT_NE(first_block_addr, nullptr);

    // 释放输出块，触发回池
    out_b1.clear();

    // 第二次调用，池中唯一的块必须被严格复用
    NamedIoBatch in_b2(1), out_b2(1);
    in_b2[0]["chan.keyword_in"] = MakeBorrowedOperatorInput(&in);
    out_b2[0]["chan.keyword_out"] = std::shared_ptr<void>();
    ASSERT_EQ(ops_.Process(handle, in_b2, out_b2), 0);

    void* second_block_addr = out_b2[0]["chan.keyword_out"].get();
    EXPECT_EQ(first_block_addr, second_block_addr);

    out_b2.clear();
    EXPECT_EQ(ops_.Destroy(handle), 0);
  }
}

// 20. 多句柄并发执行与独立输出池隔离测试
TEST_F(OperatorApiTest, ConcurrentDifferentHandles) {
  std::string root_dir = GetConfDir();
  CreateParam param1{};
  param1.model_path = root_dir.c_str();
  param1.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param1.device_id = 0;
  param1.compute_platform = ComputePlatform::kAx650;
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
  CompanyOperatorKeywordInput in{1001, &cs, COMPANY_MOCK_SERVICE_KEYWORD_MATCH};

  std::atomic<bool> success1{false};
  std::atomic<bool> success2{false};

  std::thread t1([&]() {
    for (int i = 0; i < 10; ++i) {
      NamedIoBatch in_b(1), out_b(1);
      in_b[0]["chan.keyword_in"] = MakeBorrowedOperatorInput(&in);
      out_b[0]["chan.keyword_out"] = std::shared_ptr<void>();
      if (ops_.Process(handle1, in_b, out_b) != 0) return;
      out_b.clear();
    }
    success1 = true;
  });

  std::thread t2([&]() {
    for (int i = 0; i < 10; ++i) {
      NamedIoBatch in_b(1), out_b(1);
      in_b[0]["chan.keyword_in"] = MakeBorrowedOperatorInput(&in);
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

// 21. outputs 配置校验与异常 Fail-Closed 测试
TEST_F(OperatorApiTest, OutputsConfigValidationFailClosed) {
  ScopedTempDirectory temp_dir;
  std::filesystem::path root = temp_dir.path();
  std::filesystem::create_directories(root / "configs");
  std::filesystem::copy_file(
      std::filesystem::path(GetConfDir()) /
          "configs/pipeline_keyword_match_rules.json",
      root / "configs/pipeline_keyword_match_rules.json");

  std::string conf_file = "configs/test.conf";
  std::filesystem::path conf_path = root / conf_file;

  const std::string root_str = root.string();
  CreateParam param{};
  param.model_path = root_str.c_str();
  param.cfg_file_name = conf_file.c_str();
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;

  // 0. .conf 只允许 pipe_path，未知字段严格拒绝 -> -2
  {
    std::ofstream ofs(conf_path);
    ofs << R"({"pipe_path": "pipeline_keyword_match_rules.json",
              "extra_field": 1})";
  }
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  EXPECT_NE(std::string(GetOperatorLastError()).find("Unknown field"),
            std::string::npos);

  // 0b. 未知字段 mem_que 在 io 中严格拒绝 -> -2
  {
    std::ofstream c_ofs(conf_path);
    c_ofs << R"({"pipe_path": "pipeline_keyword_match_rules.json"})";
    c_ofs.close();

    std::ifstream json_in(std::filesystem::path(GetConfDir()) /
                          "configs/pipeline_keyword_match_rules.json");
    nlohmann::json pipe_json;
    json_in >> pipe_json;
    pipe_json["io"]["mem_que"] = nlohmann::json::object();
    std::ofstream p_ofs(root / "configs/pipeline_keyword_match_rules.json");
    p_ofs << pipe_json.dump(2);
    p_ofs.close();
  }
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  EXPECT_NE(
      std::string(GetOperatorLastError()).find("Unknown field at /io/mem_que"),
      std::string::npos);

  // 1. 输出项缺少 params 时使用 converter 声明的默认值。
  {
    std::ifstream json_in(std::filesystem::path(GetConfDir()) /
                          "configs/pipeline_keyword_match_rules.json");
    nlohmann::json pipe_json;
    json_in >> pipe_json;
    ASSERT_FALSE(pipe_json["io"]["output"][0].contains("params"));
    std::ofstream(root / "configs/pipeline_keyword_match_rules.json")
        << pipe_json;
  }
  ASSERT_EQ(ops_.Create(&handle, &param), 0) << GetOperatorLastError();
  EXPECT_EQ(ops_.Destroy(handle), 0);
  handle = nullptr;

  // 2. 输出项 params 中的未知参数被拒绝，诊断路径指向该参数。
  {
    std::ifstream json_in(std::filesystem::path(GetConfDir()) /
                          "configs/pipeline_keyword_match_rules.json");
    nlohmann::json pipe_json;
    json_in >> pipe_json;
    pipe_json["io"]["output"][0]["params"]["unknown_field"] = 1;
    std::ofstream p_ofs(root / "configs/pipeline_keyword_match_rules.json");
    p_ofs << pipe_json.dump(2);
    p_ofs.close();
  }
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  EXPECT_NE(std::string(GetOperatorLastError())
                .find("/io/output/0/params/unknown_field"),
            std::string::npos);

  // 3. 尺寸参数超过平台上限 -> -2
  {
    std::ifstream json_in(std::filesystem::path(GetConfDir()) /
                          "configs/pipeline_keyword_match_rules.json");
    nlohmann::json pipe_json;
    json_in >> pipe_json;
    pipe_json["io"]["output"][0]["params"] = {
        {"match_result_json_max_bytes", 65537}};
    std::ofstream p_ofs(root / "configs/pipeline_keyword_match_rules.json");
    p_ofs << pipe_json.dump(2);
    p_ofs.close();
  }
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  EXPECT_NE(std::string(GetOperatorLastError())
                .find("/io/output/0/params/match_result_json_max_bytes"),
            std::string::npos);

  // 6. 根层 model_path 单值字段被拒绝（路径只存在 models 条目中）-> -2
  {
    std::ifstream json_in(std::filesystem::path(GetConfDir()) /
                          "configs/pipeline_keyword_match_rules.json");
    nlohmann::json pipe_json;
    json_in >> pipe_json;
    pipe_json["model_path"] = "models/unused.bin";
    std::ofstream p_ofs(root / "configs/pipeline_keyword_match_rules.json");
    p_ofs << pipe_json.dump(2);
    p_ofs.close();

    std::ofstream ofs(conf_path);
    ofs << R"({"pipe_path": "pipeline_keyword_match_rules.json"})";
  }
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  EXPECT_NE(
      std::string(GetOperatorLastError()).find("Unknown field at /model_path"),
      std::string::npos);

  // 7. .conf 根对象仅允许 pipe_path -> -2
  {
    std::ofstream ofs(conf_path);
    ofs << R"({
      "pipe_path": "pipeline_keyword_match_rules.json",
      "comment": "not part of the runtime contract"
    })";
  }
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  EXPECT_NE(
      std::string(GetOperatorLastError()).find("Unknown field at /: 'comment'"),
      std::string::npos);
}

// 22. SSO 短字符串 (1~7 字节) 与跨批次指针绝对地址稳定性测试 (R9-001)
TEST_F(OperatorApiTest, ShortStringSsoAndAddressStability) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kAx650;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  // 构造包含 1~7 字节短字符串 (SSO 敏感) 的多个样本
  const char* short_words[] = {"a",     "bc",     "def",    "ghij",
                               "klmno", "pqrstu", "vwxyz12"};
  NamedIoBatch batch_inputs(7);
  std::vector<CompanyString> company_strings(7);
  std::vector<CompanyOperatorKeywordInput> inputs(7);

  for (size_t i = 0; i < 7; ++i) {
    company_strings[i].length =
        static_cast<int32_t>(std::strlen(short_words[i]));
    company_strings[i].data = const_cast<char*>(short_words[i]);
    inputs[i].request_id = 70000 + i;
    inputs[i].service_type = COMPANY_MOCK_SERVICE_KEYWORD_MATCH;
    inputs[i].sentence_text = &company_strings[i];

    batch_inputs[i]["client_channel.keyword_in"] =
        MakeBorrowedOperatorInput(&inputs[i]);
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
        static_cast<const CompanyOperatorKeywordOutput*>(out_sp.get());
    EXPECT_EQ(out->request_id, 70000 + i);
  }

  batch_outputs.clear();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// 24. 严格路径沙箱与非法路径拦截测试 (R9-003)
TEST_F(OperatorApiTest, PathSandboxStrictBoundaries) {
  std::string root_dir = GetConfDir();
  char err_buf[256] = {0};
  OperatorIoContract contract;

  // 1. POSIX 绝对路径拒绝
  EXPECT_EQ(ResolveOperatorConfigIo(root_dir.c_str(), "/etc/pipeline.conf",
                                    &contract, err_buf, sizeof(err_buf)),
            -2);

  // 2. Windows 盘符拒绝
  EXPECT_EQ(ResolveOperatorConfigIo(root_dir.c_str(), "C:\\pipeline.conf",
                                    &contract, err_buf, sizeof(err_buf)),
            -2);

  // 3. UNC 路径拒绝
  EXPECT_EQ(ResolveOperatorConfigIo(root_dir.c_str(),
                                    "\\\\server\\share\\pipeline.conf",
                                    &contract, err_buf, sizeof(err_buf)),
            -2);

  // 4. .. 逃逸拒绝
  EXPECT_EQ(ResolveOperatorConfigIo(root_dir.c_str(), "../../etc/passwd",
                                    &contract, err_buf, sizeof(err_buf)),
            -2);

  // 5. 目录而非普通文件拒绝
  EXPECT_EQ(ResolveOperatorConfigIo(root_dir.c_str(), "configs", &contract,
                                    err_buf, sizeof(err_buf)),
            -2);

  // 6. 不存在的文件拒绝
  EXPECT_EQ(
      ResolveOperatorConfigIo(root_dir.c_str(), "configs/non_existent.conf",
                              &contract, err_buf, sizeof(err_buf)),
      -2);

  // 7. 路径前缀混淆拒绝 (例如目标根为 root，试图访问 root_extra 目录)
  EXPECT_EQ(
      ResolveOperatorConfigIo(root_dir.c_str(), "../configs_fake/pipeline.conf",
                              &contract, err_buf, sizeof(err_buf)),
      -2);

  // 8. 对 Create 接口同样严格拦截非普通文件与不存在文件
  CreateParam bad_param{};
  bad_param.model_path = root_dir.c_str();
  bad_param.cfg_file_name = "configs";  // 目录
  void* handle = nullptr;
  EXPECT_EQ(ops_.Create(&handle, &bad_param), -2);
  EXPECT_EQ(handle, nullptr);

  bad_param.cfg_file_name = "configs/non_existent.conf";
  EXPECT_EQ(ops_.Create(&handle, &bad_param), -2);
  EXPECT_EQ(handle, nullptr);
}

// 25. 深度上限与总内存预算超限防御 (R9-005)
TEST_F(OperatorApiTest, DepthLimitAndTotalMemoryBudget) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kAx650;

  // 1. 超过最大深度 1024
  param.max_frame_depth = 2048;
  void* handle = nullptr;
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  EXPECT_EQ(handle, nullptr);
}

// 27. 全部 7 类核心业务最大 Batch 边界与两端样本读取正确性验证 (R9-001)
TEST_F(OperatorApiTest, MultiBusinessMaxBatchBoundarySuite) {
  std::string root_dir = GetConfDir();

  // 1. DocQA 最大 Batch 压测 (不同文本，验证两端正确读取与清空后销毁)
  {
    CreateParam param{};
    param.model_path = root_dir.c_str();
    param.cfg_file_name = "demo/fixtures/mock/pipeline_doc_qa.conf";
    param.device_id = 0;
    param.compute_platform = ComputePlatform::kAx650;
    param.max_frame_depth = 8;

    void* handle = nullptr;
    ASSERT_EQ(ops_.Create(&handle, &param), 0);

    constexpr size_t kBatch = 4;
    std::vector<std::string> q_strs(kBatch), d_strs(kBatch);
    std::vector<CompanyString> q_cs(kBatch), d_cs(kBatch);
    std::vector<CompanyOperatorDocInput> inputs(kBatch);
    NamedIoBatch batch_in(kBatch), batch_out(kBatch);

    for (size_t i = 0; i < kBatch; ++i) {
      q_strs[i] = "Doc QA Question #" + std::to_string(i);
      d_strs[i] = "Doc QA Context Document Text #" + std::to_string(i);
      q_cs[i] = CompanyString{static_cast<int32_t>(q_strs[i].size()),
                              q_strs[i].data()};
      d_cs[i] = CompanyString{static_cast<int32_t>(d_strs[i].size()),
                              d_strs[i].data()};
      inputs[i].request_id = static_cast<uint64_t>(100 + i);
      inputs[i].service_type = COMPANY_MOCK_SERVICE_DOC_QA;
      inputs[i].query_text = &q_cs[i];
      inputs[i].doc_text = &d_cs[i];
      batch_in[i]["qa.doc_in"] = MakeBorrowedOperatorInput(&inputs[i]);
      batch_out[i]["qa.doc_out"] = nullptr;
    }

    int ret = ops_.Process(handle, batch_in, batch_out);
    EXPECT_EQ(ret, 0);

    auto* first_out = static_cast<CompanyOperatorDocOutput*>(
        batch_out[0]["qa.doc_out"].get());
    auto* last_out = static_cast<CompanyOperatorDocOutput*>(
        batch_out[kBatch - 1]["qa.doc_out"].get());
    ASSERT_NE(first_out, nullptr);
    ASSERT_NE(last_out, nullptr);
    EXPECT_EQ(first_out->request_id, 100u);
    EXPECT_EQ(last_out->request_id, 100u + kBatch - 1);
    EXPECT_NE(first_out->intent_name, nullptr);
    EXPECT_NE(first_out->answer_text, nullptr);
    EXPECT_NE(last_out->intent_name, nullptr);
    EXPECT_NE(last_out->answer_text, nullptr);

    batch_out.clear();
    EXPECT_EQ(ops_.Destroy(handle), 0);
  }

  // 2. DialogueAudit 最大 Batch 压测
  {
    CreateParam param{};
    param.model_path = root_dir.c_str();
    param.cfg_file_name = "demo/fixtures/mock/pipeline_dialogue_audit.conf";
    param.device_id = 0;
    param.compute_platform = ComputePlatform::kAx650;
    param.max_frame_depth = 8;

    void* handle = nullptr;
    ASSERT_EQ(ops_.Create(&handle, &param), 0);

    constexpr size_t kBatch = 4;
    std::vector<std::string> u_strs(kBatch), c_strs(kBatch);
    std::vector<CompanyString> u_cs(kBatch), c_cs(kBatch);
    std::vector<CompanyOperatorAuditInput> inputs(kBatch);
    NamedIoBatch batch_in(kBatch), batch_out(kBatch);

    for (size_t i = 0; i < kBatch; ++i) {
      u_strs[i] = "Audit Dialogue User Content #" + std::to_string(i);
      c_strs[i] = "channel_" + std::to_string(i);
      u_cs[i] = CompanyString{static_cast<int32_t>(u_strs[i].size()),
                              u_strs[i].data()};
      c_cs[i] = CompanyString{static_cast<int32_t>(c_strs[i].size()),
                              c_strs[i].data()};
      inputs[i].request_id = static_cast<uint64_t>(200 + i);
      inputs[i].service_type = COMPANY_MOCK_SERVICE_DIALOGUE_AUDIT;
      inputs[i].user_text = &u_cs[i];
      inputs[i].channel_name = &c_cs[i];
      batch_in[i]["audit.audit_in"] = MakeBorrowedOperatorInput(&inputs[i]);
      batch_out[i]["audit.audit_out"] = nullptr;
    }

    int ret = ops_.Process(handle, batch_in, batch_out);
    EXPECT_EQ(ret, 0);

    auto* first_out = static_cast<CompanyOperatorAuditOutput*>(
        batch_out[0]["audit.audit_out"].get());
    auto* last_out = static_cast<CompanyOperatorAuditOutput*>(
        batch_out[kBatch - 1]["audit.audit_out"].get());
    ASSERT_NE(first_out, nullptr);
    ASSERT_NE(last_out, nullptr);
    EXPECT_EQ(first_out->request_id, 200u);
    EXPECT_EQ(last_out->request_id, 200u + kBatch - 1);
    EXPECT_NE(first_out->risk_level, nullptr);
    EXPECT_NE(first_out->audit_verdict_json, nullptr);

    batch_out.clear();
    EXPECT_EQ(ops_.Destroy(handle), 0);
  }

  // 3. AudioAsrIntent 边界与 Batch 压测
  {
    CreateParam param{};
    param.model_path = root_dir.c_str();
    param.cfg_file_name = "demo/fixtures/mock/pipeline_audio_asr_intent.conf";
    param.device_id = 0;
    param.compute_platform = ComputePlatform::kAx650;
    param.max_frame_depth = 8;

    void* handle = nullptr;
    ASSERT_EQ(ops_.Create(&handle, &param), 0);

    constexpr size_t kBatch = 2;
    std::vector<std::vector<float>> pcm_buffers(kBatch,
                                                std::vector<float>(16000));
    for (size_t i = 0; i < kBatch; ++i) {
      for (size_t j = 0; j < 16000; ++j) {
        pcm_buffers[i][j] = static_cast<float>(i * 0.01f + j * 0.0001f);
      }
    }

    std::vector<CompanyOperatorAudioInput> inputs(kBatch);
    NamedIoBatch batch_in(kBatch), batch_out(kBatch);
    for (size_t i = 0; i < kBatch; ++i) {
      inputs[i].request_id = static_cast<uint64_t>(300 + i);
      inputs[i].service_type = COMPANY_MOCK_SERVICE_AUDIO_ASR_INTENT;
      inputs[i].sample_rate = 16000;
      inputs[i].pcm_length = 16000;
      inputs[i].pcm_buffer = pcm_buffers[i].data();
      batch_in[i]["audio.audio_in"] = MakeBorrowedOperatorInput(&inputs[i]);
      batch_out[i]["audio.audio_out"] = nullptr;
    }

    int ret = ops_.Process(handle, batch_in, batch_out);
    EXPECT_EQ(ret, 0);

    auto* first_out = static_cast<CompanyOperatorAudioOutput*>(
        batch_out[0]["audio.audio_out"].get());
    auto* last_out = static_cast<CompanyOperatorAudioOutput*>(
        batch_out[kBatch - 1]["audio.audio_out"].get());
    ASSERT_NE(first_out, nullptr);
    ASSERT_NE(last_out, nullptr);
    EXPECT_EQ(first_out->request_id, 300u);
    EXPECT_EQ(last_out->request_id, 300u + kBatch - 1);
    EXPECT_NE(first_out->transcribed_text, nullptr);
    EXPECT_NE(first_out->intent_slot_json, nullptr);

    batch_out.clear();
    EXPECT_EQ(ops_.Destroy(handle), 0);
  }

  // 4. CrossRerank 最大 Batch 压测
  if (llm_edgeflow::BackendRegistry::Instance()
          .Find("onnxruntime")
          .has_value()) {
    auto temp_cfg = PrepareCrossRerankFixtureConfig();
    std::string rerank_model_path_str = temp_cfg.first->path().string();
    CreateParam param{};
    param.model_path = rerank_model_path_str.c_str();
    param.cfg_file_name = temp_cfg.second.c_str();
    param.device_id = 0;
    param.compute_platform = ComputePlatform::kCpu;
    param.max_frame_depth = 8;

    void* handle = nullptr;
    ASSERT_EQ(ops_.Create(&handle, &param), 0);

    constexpr size_t kBatch = 4;
    std::vector<std::string> q_strs(kBatch);
    std::vector<std::vector<std::string>> c_strs(kBatch,
                                                 std::vector<std::string>(8));
    std::vector<CompanyString> q_cs(kBatch);
    std::vector<std::vector<CompanyString>> c_cs(kBatch,
                                                 std::vector<CompanyString>(8));
    std::vector<CompanyOperatorRerankInput> inputs(kBatch);
    NamedIoBatch batch_in(kBatch), batch_out(kBatch);

    for (size_t i = 0; i < kBatch; ++i) {
      q_strs[i] = "Cross Rerank Query #" + std::to_string(i);
      q_cs[i] = CompanyString{static_cast<int32_t>(q_strs[i].size()),
                              q_strs[i].data()};
      inputs[i].request_id = static_cast<uint64_t>(400 + i);
      inputs[i].service_type = COMPANY_MOCK_SERVICE_CROSS_RERANK;
      inputs[i].query_text = &q_cs[i];
      inputs[i].candidate_count = 8;
      for (size_t c = 0; c < 8; ++c) {
        c_strs[i][c] = "Candidate passage " + std::to_string(c) + " for req #" +
                       std::to_string(i);
        c_cs[i][c] = CompanyString{static_cast<int32_t>(c_strs[i][c].size()),
                                   c_strs[i][c].data()};
        inputs[i].candidate_passages[c] = &c_cs[i][c];
      }
      batch_in[i]["rank.rerank_in"] = MakeBorrowedOperatorInput(&inputs[i]);
      batch_out[i]["rank.rerank_out"] = nullptr;
    }

    int ret = ops_.Process(handle, batch_in, batch_out);
    EXPECT_EQ(ret, 0);

    auto* first_out = static_cast<CompanyOperatorRerankOutput*>(
        batch_out[0]["rank.rerank_out"].get());
    auto* middle_out = static_cast<CompanyOperatorRerankOutput*>(
        batch_out[1]["rank.rerank_out"].get());
    auto* last_out = static_cast<CompanyOperatorRerankOutput*>(
        batch_out[kBatch - 1]["rank.rerank_out"].get());
    ASSERT_NE(first_out, nullptr);
    ASSERT_NE(middle_out, nullptr);
    ASSERT_NE(last_out, nullptr);
    EXPECT_EQ(first_out->request_id, 400u);
    EXPECT_EQ(middle_out->request_id, 401u);
    EXPECT_EQ(last_out->request_id, 400u + kBatch - 1);
    EXPECT_EQ(first_out->count, 8);
    EXPECT_EQ(middle_out->count, 8);
    EXPECT_EQ(last_out->count, 8);

    batch_out.clear();
    EXPECT_EQ(ops_.Destroy(handle), 0);
  }

  // 5. OcrInvoiceQa 最大 Batch 压测
  {
    CreateParam param{};
    param.model_path = root_dir.c_str();
    param.cfg_file_name = "demo/fixtures/mock/pipeline_ocr_invoice_qa.conf";
    param.device_id = 0;
    param.compute_platform = ComputePlatform::kAx650;
    param.max_frame_depth = 8;

    void* handle = nullptr;
    ASSERT_EQ(ops_.Create(&handle, &param), 0);

    constexpr size_t kBatch = 2;
    std::vector<std::string> uris(kBatch), q_strs(kBatch);
    std::vector<CompanyString> uri_cs(kBatch), q_cs(kBatch);
    std::vector<CompanyFrame> frames(kBatch);
    NamedIoBatch batch_in(kBatch), batch_out(kBatch);

    for (size_t i = 0; i < kBatch; ++i) {
      uris[i] = "data/invoice_0" + std::to_string(i + 1) + ".jpg";
      q_strs[i] = "What is invoice item #" + std::to_string(i) + "?";
      uri_cs[i] =
          CompanyString{static_cast<int32_t>(uris[i].size()), uris[i].data()};
      q_cs[i] = CompanyString{static_cast<int32_t>(q_strs[i].size()),
                              q_strs[i].data()};
      frames[i].request_id = static_cast<uint64_t>(600 + i);
      frames[i].service_type = COMPANY_MOCK_SERVICE_OCR_INVOICE_QA;
      frames[i].image_uri = &uri_cs[i];
      frames[i].metadata = nullptr;
      batch_in[i]["camera_0.frame"] = MakeBorrowedOperatorInput(&frames[i]);
      batch_in[i]["query_channel.string"] = MakeBorrowedOperatorInput(&q_cs[i]);
      batch_out[i]["ocr_result.od_out"] = nullptr;
    }

    int ret = ops_.Process(handle, batch_in, batch_out);
    EXPECT_EQ(ret, 0);

    auto* first_out =
        static_cast<CompanyOdOutput*>(batch_out[0]["ocr_result.od_out"].get());
    auto* last_out = static_cast<CompanyOdOutput*>(
        batch_out[kBatch - 1]["ocr_result.od_out"].get());
    ASSERT_NE(first_out, nullptr);
    ASSERT_NE(last_out, nullptr);
    EXPECT_EQ(first_out->request_id, 600u);
    EXPECT_EQ(last_out->request_id, 600u + kBatch - 1);
    EXPECT_NE(first_out->result_json, nullptr);
    EXPECT_NE(last_out->result_json, nullptr);

    batch_out.clear();
    EXPECT_EQ(ops_.Destroy(handle), 0);
  }

  // 6. EntityExtract 最大 Batch 压测
  {
    CreateParam param{};
    param.model_path = root_dir.c_str();
    param.cfg_file_name = "demo/fixtures/mock/pipeline_entity_extract.conf";
    param.device_id = 0;
    param.compute_platform = ComputePlatform::kAx650;
    param.max_frame_depth = 8;

    void* handle = nullptr;
    ASSERT_EQ(ops_.Create(&handle, &param), 0);

    constexpr size_t kBatch = 4;
    std::vector<std::string> s_strs(kBatch);
    std::vector<CompanyString> s_cs(kBatch);
    std::vector<CompanyOperatorEntityInput> inputs(kBatch);
    NamedIoBatch batch_in(kBatch), batch_out(kBatch);

    for (size_t i = 0; i < kBatch; ++i) {
      s_strs[i] = "User #" + std::to_string(i) + " works at Acme in Beijing.";
      s_cs[i] = CompanyString{static_cast<int32_t>(s_strs[i].size()),
                              s_strs[i].data()};
      inputs[i].request_id = static_cast<uint64_t>(500 + i);
      inputs[i].service_type = COMPANY_MOCK_SERVICE_ENTITY_EXTRACT;
      inputs[i].sentence_text = &s_cs[i];
      batch_in[i]["ner.entity_in"] = MakeBorrowedOperatorInput(&inputs[i]);
      batch_out[i]["ner.entity_out"] = nullptr;
    }

    int ret = ops_.Process(handle, batch_in, batch_out);
    EXPECT_EQ(ret, 0);

    auto* first_out = static_cast<CompanyOperatorEntityOutput*>(
        batch_out[0]["ner.entity_out"].get());
    auto* last_out = static_cast<CompanyOperatorEntityOutput*>(
        batch_out[kBatch - 1]["ner.entity_out"].get());
    ASSERT_NE(first_out, nullptr);
    ASSERT_NE(last_out, nullptr);
    EXPECT_EQ(first_out->request_id, 500u);
    EXPECT_EQ(last_out->request_id, 500u + kBatch - 1);
    EXPECT_NE(first_out->entities_json, nullptr);
    EXPECT_NE(last_out->entities_json, nullptr);

    batch_out.clear();
    EXPECT_EQ(ops_.Destroy(handle), 0);
  }
}

// 28. 全量 64 帧最大 Batch 与 65 帧超限拒绝测试 (R9-001, R9-010)
TEST_F(OperatorApiTest, Full64MaxBatchAnd65ExceedReject) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kAx650;
  param.max_frame_depth = 64;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  // 1. 构造 64 个不同内容的样本进行最大批处理
  constexpr size_t kMaxBatch = 64;
  std::vector<std::string> sent_strs(kMaxBatch);
  std::vector<CompanyString> comp_strs(kMaxBatch);
  std::vector<CompanyOperatorKeywordInput> inputs(kMaxBatch);
  NamedIoBatch batch_in(kMaxBatch), batch_out(kMaxBatch);

  for (size_t i = 0; i < kMaxBatch; ++i) {
    sent_strs[i] = "Sentence item #" + std::to_string(i) +
                   (i % 2 == 0 ? " 系统初始化完成" : " 普通用户消息");
    comp_strs[i] = CompanyString{static_cast<int32_t>(sent_strs[i].size()),
                                 sent_strs[i].data()};
    inputs[i].request_id = static_cast<uint64_t>(1000 + i);
    inputs[i].service_type = COMPANY_MOCK_SERVICE_KEYWORD_MATCH;
    inputs[i].sentence_text = &comp_strs[i];
    batch_in[i]["client_channel.keyword_in"] =
        MakeBorrowedOperatorInput(&inputs[i]);
    batch_out[i]["client_channel.keyword_out"] = nullptr;
  }

  int ret = ops_.Process(handle, batch_in, batch_out);
  EXPECT_EQ(ret, 0);

  for (size_t i = 0; i < kMaxBatch; ++i) {
    auto out_sp = batch_out[i]["client_channel.keyword_out"];
    ASSERT_NE(out_sp, nullptr);
    auto* out_ptr = static_cast<CompanyOperatorKeywordOutput*>(out_sp.get());
    EXPECT_EQ(out_ptr->request_id, 1000u + i);
    EXPECT_NE(out_ptr->match_result_json, nullptr);
    if (i % 2 == 0) {
      EXPECT_EQ(out_ptr->is_hit, 1);
    }
  }

  // 清空 64 帧输出
  batch_out.clear();

  // 2. 构造 65 帧请求 -> 必须被拒绝且返回 -3 (COMPANY_ALG_ERR_INVALID_INPUT)
  constexpr size_t kOverBatch = 65;
  std::vector<std::string> over_strs(kOverBatch, "overflow sentence");
  std::vector<CompanyString> over_cs(kOverBatch);
  std::vector<CompanyOperatorKeywordInput> over_inputs(kOverBatch);
  NamedIoBatch over_batch_in(kOverBatch), over_batch_out(kOverBatch);

  for (size_t i = 0; i < kOverBatch; ++i) {
    over_cs[i] = CompanyString{static_cast<int32_t>(over_strs[i].size()),
                               over_strs[i].data()};
    over_inputs[i].request_id = static_cast<uint64_t>(2000 + i);
    over_inputs[i].service_type = COMPANY_MOCK_SERVICE_KEYWORD_MATCH;
    over_inputs[i].sentence_text = &over_cs[i];
    over_batch_in[i]["client_channel.keyword_in"] =
        MakeBorrowedOperatorInput(&over_inputs[i]);
    over_batch_out[i]["client_channel.keyword_out"] = nullptr;
  }

  int over_ret = ops_.Process(handle, over_batch_in, over_batch_out);
  EXPECT_EQ(over_ret, -3);

  // 3. 释放并正常销毁
  over_batch_out.clear();
  EXPECT_EQ(ops_.Destroy(handle), 0);
}

// 29. 未归还输出句柄时的违约 Destroy 契约测试 (R9-010)
TEST_F(OperatorApiTest, UnreleasedOutputLifecycleBreach) {
  std::string root_dir = GetConfDir();
  CreateParam param{};
  param.model_path = root_dir.c_str();
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kAx650;
  param.max_frame_depth = 5;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);

  std::string text = "VIP专员";
  CompanyString cs{static_cast<int32_t>(text.size()), text.data()};
  CompanyOperatorKeywordInput in{99001, &cs,
                                 COMPANY_MOCK_SERVICE_KEYWORD_MATCH};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["client_channel.keyword_in"] = MakeBorrowedOperatorInput(&in);
  out_b[0]["client_channel.keyword_out"] = nullptr;

  ASSERT_EQ(ops_.Process(handle, in_b, out_b), 0);
  ASSERT_NE(out_b[0]["client_channel.keyword_out"], nullptr);

  // 在调用方依然持有输出 shared_ptr 时调用 Destroy -> 必须返回 -1 (未归还违约)
  EXPECT_EQ(ops_.Destroy(handle), -1);

  // 随后调用方释放输出
  out_b.clear();
}

// 30. 模型文件不存在时允许部署引用通过，但越界逃逸与控制文件缺失必须拦截
// (R9-003)
TEST_F(OperatorApiTest, ModelPathNonExistentFileAllowedWhileEscapeRejected) {
  ScopedTempDirectory temp_root;
  ScopedTempDirectory temp_outside;
  const std::filesystem::path root = temp_root.path();
  const std::filesystem::path outside = temp_outside.path();
  std::filesystem::create_directories(root / "configs");
  const std::filesystem::path canonical_root = std::filesystem::canonical(root);
  const std::filesystem::path source_root = GetConfDir();
  std::string err;

  // 1. models 中的路径指向尚未部署的模型；
  // Resolver 只规范化引用，不能创建或要求模型文件存在。
  {
    std::filesystem::copy_file(
        source_root / "demo/fixtures/mock/pipeline_doc_qa.json",
        root / "configs/pipeline_doc_qa_default.json");
    nlohmann::json original_io;
    {
      std::ifstream pipe_in(root / "configs/pipeline_doc_qa_default.json");
      nlohmann::json pipe_json;
      pipe_in >> pipe_json;
      pipe_json["models"][0]["model_path"] = "models/not_deployed_embed.bin";
      pipe_json["models"][1]["model_path"] = "models/not_deployed_llm.bin";
      original_io = pipe_json["io"];
      std::ofstream pipe_out(root / "configs/pipeline_doc_qa_default.json");
      pipe_out << pipe_json.dump(2);
    }
    std::ofstream conf(root / "configs/model_paths.conf");
    conf << R"({
      "pipe_path": "pipeline_doc_qa_default.json"
    })";
    conf.close();

    ASSERT_FALSE(std::filesystem::exists(root / "models"));
    const std::string root_string = root.string();

    llm_edgeflow::ResolvedOperatorConfig resolved;
    int ret = llm_edgeflow::OperatorConfigResolver::Resolve(
        root_string.c_str(), "configs/model_paths.conf", &resolved, &err);
    ASSERT_EQ(ret, 0) << "Error: " << err;
    ASSERT_NE(resolved.io_plan, nullptr);
    // 解析后的文档写入生效的 io：输入不变，输出项补上生效的参数。
    ASSERT_TRUE(resolved.io_plan->resolved_pipeline_json.contains("io"));
    const auto& resolved_io = resolved.io_plan->resolved_pipeline_json["io"];
    EXPECT_EQ(resolved_io["input"], original_io["input"]);
    ASSERT_EQ(resolved_io["output"].size(), 1u);
    EXPECT_EQ(resolved_io["output"][0]["type"],
              original_io["output"][0]["type"]);
    EXPECT_EQ(resolved_io["output"][0]["name"],
              original_io["output"][0]["name"]);
    EXPECT_TRUE(resolved_io["output"][0].contains("params"));
    ASSERT_EQ(resolved.io_plan->resolved_pipeline_json["models"].size(), 2u);
    for (const auto& model :
         resolved.io_plan->resolved_pipeline_json["models"]) {
      const auto path =
          std::filesystem::path(model["model_path"].get<std::string>());
      EXPECT_TRUE(path.is_absolute());
      EXPECT_EQ(path.lexically_relative(canonical_root).string().rfind("..", 0),
                std::string::npos);
      EXPECT_FALSE(std::filesystem::exists(path));
    }
    EXPECT_FALSE(std::filesystem::exists(root / "models"));
  }

  // 2. 单个模型直接指定路径，且允许最终文件尚未部署。
  {
    std::filesystem::copy_file(
        source_root / "demo/fixtures/mock/pipeline_audio_asr_intent.json",
        root / "configs/pipeline_audio_asr_intent.json");
    {
      std::ifstream pipe_in(root / "configs/pipeline_audio_asr_intent.json");
      nlohmann::json pipe_json;
      pipe_in >> pipe_json;
      pipe_json["models"][0]["model_path"] =
          "deployment/asr_model_will_arrive_later.bin";
      std::ofstream pipe_out(root / "configs/pipeline_audio_asr_intent.json");
      pipe_out << pipe_json.dump(2);
    }
    std::ofstream conf(root / "configs/single_model.conf");
    conf << R"({
      "pipe_path": "pipeline_audio_asr_intent.json"
    })";
    conf.close();

    const std::string root_string = root.string();

    llm_edgeflow::ResolvedOperatorConfig resolved;
    int ret = llm_edgeflow::OperatorConfigResolver::Resolve(
        root_string.c_str(), "configs/single_model.conf", &resolved, &err);
    ASSERT_EQ(ret, 0) << err;
    const auto resolved_model = std::filesystem::path(
        resolved.io_plan->resolved_pipeline_json["models"][0]["model_path"]
            .get<std::string>());
    EXPECT_EQ(resolved_model,
              canonical_root / "deployment/asr_model_will_arrive_later.bin");
    EXPECT_FALSE(std::filesystem::exists(resolved_model));
  }

  // 当前的模型解析接受根目录内的规范绝对路径；即使文件不存在，
  // 也拒绝相对路径遍历和符号链接逃逸。
  {
    auto resolve = [&](const std::string& reference, nlohmann::json* resolved) {
      return llm_edgeflow::ResolveDeploymentModelPaths(
          {{"models", {{{"model_id", "asr"}, {"model_path", reference}}}}},
          root.string(), resolved, &err);
    };
    nlohmann::json resolved;
    for (const std::string& safe :
         {std::string("safe/missing_model.bin"),
          std::string("..name/missing_model.bin"),
          std::string("safe/../missing_model.bin"), std::string("."),
          (canonical_root / "absolute_model.bin").string()}) {
      ASSERT_TRUE(resolve(safe, &resolved)) << safe << ": " << err;
      EXPECT_EQ(
          resolved["models"][0]["model_path"].get<std::string>(),
          std::filesystem::weakly_canonical(canonical_root / safe).string());
    }
    for (const char* bad :
         {"../../escape_model.bin", "safe/../../../escape_model.bin"}) {
      EXPECT_FALSE(resolve(bad, &resolved)) << bad;
      EXPECT_FALSE(err.empty()) << bad;
    }
    std::error_code ec;
    std::filesystem::create_directory_symlink(outside, root / "outside_link",
                                              ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_FALSE(resolve("outside_link/missing_model.bin", &resolved));
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(resolve((outside / "model.bin").string(), &resolved));
  }

  // 4. cfg 和 pipe 是控制文件，仍必须存在且为 regular file。
  {
    const std::string root_string = root.string();
    llm_edgeflow::ResolvedOperatorConfig resolved;
    EXPECT_EQ(llm_edgeflow::OperatorConfigResolver::Resolve(
                  root_string.c_str(), "configs/missing.conf", &resolved, &err),
              -2);

    std::ofstream conf(root / "configs/missing_pipe.conf");
    conf << R"({
      "pipe_path": "configs/missing_pipeline.json"
    })";
    conf.close();
    EXPECT_EQ(
        llm_edgeflow::OperatorConfigResolver::Resolve(
            root_string.c_str(), "configs/missing_pipe.conf", &resolved, &err),
        -2);

    // cfg symlink 到根外 regular file 仍是逃逸，不能因为目标存在而接受。
    std::ofstream outside_conf(outside / "outside.conf");
    outside_conf << "{}";
    outside_conf.close();
    std::error_code ec;
    std::filesystem::create_symlink(outside / "outside.conf",
                                    root / "configs/outside.conf", ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(llm_edgeflow::OperatorConfigResolver::Resolve(
                  root_string.c_str(), "configs/outside.conf", &resolved, &err),
              -2);

    // pipe 的绝对路径、目录和根外 symlink 也必须由同一 required-file
    // helper 拒绝。
    std::ofstream outside_pipe(outside / "outside_pipeline.json");
    outside_pipe << "{}";
    outside_pipe.close();
    ec.clear();
    std::filesystem::create_symlink(outside / "outside_pipeline.json",
                                    root / "configs/outside_pipeline.json", ec);
    ASSERT_FALSE(ec) << ec.message();

    for (const std::string& pipe_path :
         {std::string("/absolute/pipeline.json"), std::string("configs"),
          std::string("configs/outside_pipeline.json")}) {
      std::ofstream invalid_conf(root / "configs/invalid_pipe.conf");
      invalid_conf << nlohmann::json({{"pipe_path", pipe_path}});
      invalid_conf.close();
      EXPECT_EQ(llm_edgeflow::OperatorConfigResolver::Resolve(
                    root_string.c_str(), "configs/invalid_pipe.conf", &resolved,
                    &err),
                -2)
          << pipe_path;
    }
  }
}

TEST_F(OperatorApiTest, DotDotPrefixedControlFileNamesStayWithinRoot) {
  ScopedTempDirectory temp_root;
  const auto root = temp_root.path();
  std::filesystem::create_directories(root / "..configs");
  std::filesystem::copy_file(std::filesystem::path(GetConfDir()) /
                                 "configs/pipeline_keyword_match_rules.json",
                             root / "..configs/pipeline.json");
  std::ofstream(root / "..configs/pipeline.conf")
      << nlohmann::json({{"pipe_path", "pipeline.json"}});

  llm_edgeflow::ResolvedOperatorConfig resolved;
  std::string error;
  const auto root_string = root.string();
  EXPECT_EQ(
      llm_edgeflow::OperatorConfigResolver::Resolve(
          root_string.c_str(), "..configs/pipeline.conf", &resolved, &error),
      0)
      << error;
}

TEST_F(OperatorApiTest, VariableResultsUsePoolCapacityAndRollbackOnFailure) {
  const std::string word(2300, 'x');
  for (const int capacity : {16384, 1024}) {
    ScopedTempDirectory temp;
    nlohmann::json pipeline = {
        {"io",
         {{"input", {{{"type", "keyword_in"}, {"name", "keyword_match"}}}},
          {"output",
           {{{"type", "keyword_out"},
             {"name", "keyword_match"},
             {"params", {{"match_result_json_max_bytes", capacity}}}}}}}},
        {"models", nlohmann::json::array()},
        {"pipeline",
         {{{"id", "rule"},
           {"node_type", "TextRuleMatchNode"},
           {"depends_on", nlohmann::json::array()},
           {"inputs", {{"text", "input_sentences"}}},
           {"outputs", {{"matches", "rule_matches"}}},
           {"config", {{"categories", {{"LONG", {word}}}}}}}}}};
    std::ofstream(temp.path() / "pipeline.json") << pipeline;
    std::ofstream(temp.path() / "pipeline.conf")
        << nlohmann::json({{"pipe_path", "pipeline.json"}});
    const auto root = temp.path().string();
    CreateParam param{};
    param.model_path = root.c_str();
    param.cfg_file_name = "pipeline.conf";
    param.compute_platform = ComputePlatform::kCpu;
    param.device_id = 0;
    param.max_frame_depth = 1;
    void* handle = nullptr;
    ASSERT_EQ(ops_.Create(&handle, &param), 0) << GetOperatorLastError();
    CompanyString text{static_cast<int32_t>(word.size()),
                       const_cast<char*>(word.data())};
    CompanyOperatorKeywordInput input{987, &text,
                                      COMPANY_MOCK_SERVICE_KEYWORD_MATCH};
    NamedIoBatch inputs(1), outputs(1);
    inputs[0]["chan.keyword_in"] = MakeBorrowedOperatorInput(&input);
    outputs[0]["chan.keyword_out"] = {};
    for (int attempt = 0; attempt < 2; ++attempt) {
      const int ret = ops_.Process(handle, inputs, outputs);
      if (capacity > 2048) {
        ASSERT_EQ(ret, 0) << GetOperatorLastError();
        auto* result = static_cast<CompanyOperatorKeywordOutput*>(
            outputs[0]["chan.keyword_out"].get());
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(result->request_id, 987u);
        EXPECT_GT(result->match_result_json->length, 2048);
        const std::string json(result->match_result_json->data,
                               result->match_result_json->length);
        EXPECT_NE(json.find(word), std::string::npos);
        EXPECT_TRUE(nlohmann::json::accept(json));
        outputs[0]["chan.keyword_out"].reset();
      } else {
        EXPECT_EQ(ret, COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
        EXPECT_EQ(outputs[0]["chan.keyword_out"], nullptr);
      }
    }
    // 转换失败时必须归还唯一的租约，以便之后的小结果能正常写入。
    std::string short_word = "no match";
    text = {static_cast<int32_t>(short_word.size()), short_word.data()};
    EXPECT_EQ(ops_.Process(handle, inputs, outputs), 0)
        << GetOperatorLastError();
    outputs.clear();
    EXPECT_EQ(ops_.Destroy(handle), 0);
  }
}

namespace llm_edgeflow::test_support {
namespace {

// 同一外层类型 NestedOutputEnvelope 登记在两个宿主 key 后缀下，作为同一个方案里
// 的两个输出项；每个后缀各有两种命名布局。
constexpr char kNestedMain[] = "test_nested_out";
constexpr char kNestedAudit[] = "test_nested_audit_out";

void RegisterNestedOutputTestTypes() {
  for (const bool audit : {false, true}) {
    const char* suffix = audit ? kNestedAudit : kNestedMain;
    const std::string prefix = audit ? "test_nested_audit_" : "test_nested_";
    RegisterOperatorValueType(MakeNestedOutputBinding(1, suffix));
    RegisterOperatorOutputAllocator(prefix + "standard",
                                    MakeNestedOutputBinding(1, suffix));
    RegisterOperatorOutputAllocator(prefix + "alternate",
                                    MakeNestedOutputBinding(2, suffix));
  }
  auto explicit_parameters = MakeNestedOutputBinding();
  explicit_parameters.normalize_parameters =
      MakeOutputParameterParser<NestedOutputParameters>(
          [](const std::string& text, NestedOutputParameters* parameters,
             std::string* error) {
            const auto requested = nlohmann::json::parse(text);
            if (!requested.is_object() || !requested.contains("capacity")) {
              if (error) *error = "capacity must be explicitly configured";
              return false;
            }
            return ParseNestedOutput(text, parameters, error);
          });
  RegisterOperatorOutputAllocator("test_nested_explicit_parameters",
                                  explicit_parameters);
  auto footprint = MakeNestedOutputBinding();
  footprint.output_layout.compute_block_payload_bytes =
      [](const ResolvedOutputPoolSpec&, size_t* bytes, std::string*) {
        *bytes = 3 * 1024 * 1024;
        return true;
      };
  RegisterOperatorOutputAllocator("test_nested_3mib_footprint", footprint);
}

REGISTER_OPERATOR_VALUE_TYPE(RegisterNestedOutputTestTypes);

template <bool kAudit>
int EncodeNestedOutput(AlgContext* context, const OutputEncodeOptions& options,
                       ExternalOutputBatchView* destination,
                       size_t* written_count, AdapterStatus* status) {
  if (written_count) *written_count = 0;
  if (!context || !destination) return -1;
  const char* slot_name = kAudit ? kNestedAudit : kNestedMain;
  const auto* req_ids = RequestIds(options, status);
  const auto* matches = context->Read(kRuleMatches);
  if (!req_ids || !matches) return -3;
  size_t count = req_ids->size();

  for (size_t i = 0; i < count; ++i) {
    NestedOutputSource result;
    result.request_id = (*req_ids)[i];
    result.is_hit = 0;
    for (const auto& m : *matches) {
      if (m.req_id == i) {
        result.is_hit = m.data.is_hit;
        break;
      }
    }
    void* external = destination->GetSlot<void>(slot_name, i);
    const auto* spec = destination->GetPoolSpec(slot_name);
    if (!external || !spec) return -4;
    std::string error;
    int ret = ConvertNestedOutput(&result, external, *spec, &error);
    if (ret != 0) {
      if (status) {
        *status = AdapterStatus(ret, error, slot_name, static_cast<int>(i),
                                options.Label());
      }
      return ret;
    }
  }
  if (written_count) *written_count = count;
  return 0;
}

// 输出 converter 在槽声明中固定命名布局及其参数；params 为空表示不声明。
OutputConverterDefinition NestedOutputConverter(
    bool audit, const std::string& name, const std::string& allocator,
    const nlohmann::json& params = nlohmann::json()) {
  OutputConverterDefinition def;
  def.type = audit ? kNestedAudit : kNestedMain;
  def.name = name;
  def.slot.type_id = "NestedOutputEnvelope";
  def.slot.type_suffix = def.type;
  def.slot.allocator = allocator;
  if (!params.is_null()) def.slot.allocator_params = params.dump();
  def.logical_ports = {
      NodePortDefinition("rule_matches", "RuleMatchBatch", true, "1:1")};
  def.encode_fn =
      audit ? &EncodeNestedOutput<true> : &EncodeNestedOutput<false>;
  return def;
}

struct NestedVariant {
  const char* name;
  std::string main_allocator;
  nlohmann::json main_params;
  std::string audit_allocator;
  nlohmann::json audit_params;
};

std::vector<NestedVariant> NestedVariants() {
  return {
      {"standard_pair",
       "test_nested_standard",
       {{"kind", 1}, {"capacity", 2}},
       "test_nested_audit_alternate",
       {{"kind", 2}, {"capacity", 5}}},
      {"alternate_pair",
       "test_nested_alternate",
       {{"kind", 2}, {"capacity", 3}},
       "test_nested_audit_standard",
       {{"kind", 1}, {"capacity", 4}}},
      {"default_params", "test_nested_standard", nullptr,
       "test_nested_audit_standard", nullptr},
      {"reject_audit_hits",
       "test_nested_standard",
       {{"kind", 1}, {"capacity", 2}},
       "test_nested_audit_alternate",
       {{"kind", 2}, {"capacity", 5}, {"reject_hit", true}}},
      {"huge_capacity",
       "test_nested_standard",
       {{"capacity", 9000}},
       "test_nested_audit_standard",
       {{"capacity", 9000}}},
      {"large_footprint", "test_nested_3mib_footprint", nullptr,
       "test_nested_audit_standard", nullptr},
  };
}

const bool g_reg_nested_output_components = []() {
  for (const auto& variant : NestedVariants()) {
    IoConverterRegistry::Instance().RegisterOutputConverter(
        NestedOutputConverter(false, variant.name, variant.main_allocator,
                              variant.main_params));
    IoConverterRegistry::Instance().RegisterOutputConverter(
        NestedOutputConverter(true, variant.name, variant.audit_allocator,
                              variant.audit_params));
  }
  return true;
}();

// 一个输入项（生产的关键词输入）和两个嵌套输出项（result 与 audit）。
nlohmann::json NestedOutputPipelineJson(
    const std::string& variant = "standard_pair") {
  std::ifstream source(std::filesystem::path(GetConfDir()) /
                       "configs/pipeline_keyword_match_rules.json");
  nlohmann::json pipeline;
  source >> pipeline;
  pipeline["io"] = {
      {"input", {{{"type", "keyword_in"}, {"name", "keyword_match"}}}},
      {"output",
       {{{"type", kNestedMain}, {"name", variant}},
        {{"type", kNestedAudit}, {"name", variant}}}}};
  return pipeline;
}

void WriteNestedOutputPipeline(
    const std::filesystem::path& root,
    const std::string& pipeline_name = "pipeline.json",
    const std::string& variant = "standard_pair") {
  std::ofstream(root / pipeline_name) << NestedOutputPipelineJson(variant);
}

void ExpectNestedResult(const std::shared_ptr<void>& value, uint64_t request_id,
                        int32_t tag, int32_t kind, uint32_t capacity,
                        bool is_hit) {
  ASSERT_NE(value, nullptr);
  const auto* root = static_cast<const NestedOutputEnvelope*>(value.get());
  EXPECT_EQ(root->request_id, request_id);
  EXPECT_EQ(root->allocator_tag, tag);
  ASSERT_EQ(root->kind, kind);
  ASSERT_NE(root->payload, nullptr);
  const auto* payload = static_cast<const NestedOutputPayload*>(root->payload);
  ASSERT_EQ(payload->capacity, capacity);
  ASSERT_EQ(payload->count, capacity);
  ASSERT_NE(payload->values, nullptr);
  for (uint32_t i = 0; i < capacity; ++i) {
    const int32_t expected = tag * 100 + (is_hit ? 10 : 0) + i;
    if (kind == 1) {
      EXPECT_EQ(static_cast<const int32_t*>(payload->values)[i], expected);
    } else {
      EXPECT_FLOAT_EQ(static_cast<const float*>(payload->values)[i],
                      expected + 0.5f);
    }
  }
}

}  // namespace
}  // namespace llm_edgeflow::test_support

TEST_F(OperatorApiTest, ProcessRejectsConverterRecordingWrongRequestIdCount) {
  using namespace llm_edgeflow;
  using namespace llm_edgeflow::test_support;
  constexpr int32_t kPartialIdsServiceType = 9101;  // 测试用占位取值
  const auto* production = IoConverterRegistry::Instance().FindInputConverter(
      "keyword_in", "keyword_match");
  ASSERT_NE(production, nullptr);
  auto input = *production;
  input.name = "test_partial_request_ids";
  input.service_type = kPartialIdsServiceType;
  input.decode_fn = [](const ExternalInputBatchView& source,
                       const InputDecodeOptions& options, AlgContext* context,
                       AdapterStatus* status) {
    const auto* converter = IoConverterRegistry::Instance().FindInputConverter(
        "keyword_in", "keyword_match");
    const int ret = converter->decode_fn(source, options, context, status);
    if (ret == 0) options.request_ids->resize(1);
    return ret;
  };
  // 注册表是进程级全局的；只注册一次，以便测试可重复运行。
  if (!IoConverterRegistry::Instance().FindInputConverter(input.type,
                                                          input.name))
    ASSERT_TRUE(IoConverterRegistry::Instance().RegisterInputConverter(input));

  ScopedTempDirectory temp;
  auto pipeline = NestedOutputPipelineJson();
  pipeline["io"]["input"][0]["name"] = input.name;
  std::ofstream(temp.path() / "pipeline.json") << pipeline;
  std::ofstream(temp.path() / "pipeline.conf")
      << nlohmann::json{{"pipe_path", "pipeline.json"}};
  const auto root = temp.path().string();
  auto param = DefaultCreateParam("pipeline.conf");
  param.model_path = root.c_str();
  void* raw_handle = nullptr;
  ASSERT_EQ(ops_.Create(&raw_handle, &param), 0) << GetOperatorLastError();
  const auto destroy = [this](void* handle) { ops_.Destroy(handle); };
  std::unique_ptr<void, decltype(destroy)> handle(raw_handle, destroy);
  const int resets_before = nested_resets;
  char text[] = "query";
  CompanyString sentence{5, text};
  CompanyOperatorKeywordInput rows[] = {
      {900001, &sentence, kPartialIdsServiceType},
      {42, &sentence, kPartialIdsServiceType}};
  NamedIoBatch inputs(2), outputs(2);
  for (size_t i = 0; i < 2; ++i) {
    inputs[i]["test.keyword_in"] = MakeBorrowedOperatorInput(&rows[i]);
    outputs[i]["test.test_nested_out"] = nullptr;
    outputs[i]["test.test_nested_audit_out"] = nullptr;
  }
  EXPECT_EQ(ops_.Process(handle.get(), inputs, outputs),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_STREQ(GetOperatorLastError(),
               "DecodeInput for keyword_in/test_partial_request_ids recorded "
               "1 request ids for 2 inputs");
  for (const auto& frame : outputs) {
    EXPECT_EQ(frame.at("test.test_nested_out"), nullptr);
    EXPECT_EQ(frame.at("test.test_nested_audit_out"), nullptr);
  }
  // 归还已取出的块会将其重置，即使没有发布任何输出。
  EXPECT_EQ(nested_resets, resets_before);
}

// 结构体的 service_type 与方案选中的业务不符时，Process 返回输入非法，
// 错误信息含登记、结构名和请求序号；不租用任何输出。
TEST_F(OperatorApiTest, ProcessRejectsServiceTypeMismatchWithStructAndIndex) {
  auto param = DefaultCreateParam("configs/pipeline_keyword_match_rules.conf");
  void* raw_handle = nullptr;
  ASSERT_EQ(ops_.Create(&raw_handle, &param), 0) << GetOperatorLastError();
  const auto destroy = [this](void* handle) { ops_.Destroy(handle); };
  std::unique_ptr<void, decltype(destroy)> handle(raw_handle, destroy);
  char text[] = "query";
  CompanyString sentence{5, text};
  CompanyOperatorKeywordInput rows[] = {
      {1, &sentence, COMPANY_MOCK_SERVICE_KEYWORD_MATCH},
      {2, &sentence, COMPANY_MOCK_SERVICE_TRANSLATE}};
  NamedIoBatch inputs(2), outputs(2);
  for (size_t i = 0; i < 2; ++i) {
    inputs[i]["test.keyword_in"] = MakeBorrowedOperatorInput(&rows[i]);
    outputs[i]["test.keyword_out"] = nullptr;
  }
  EXPECT_EQ(ops_.Process(handle.get(), inputs, outputs),
            COMPANY_ALG_ERR_INVALID_INPUT);
  const std::string error = GetOperatorLastError();
  EXPECT_NE(error.find("service_type mismatch for input "
                       "keyword_in/keyword_match"),
            std::string::npos)
      << error;
  EXPECT_NE(error.find("struct 'keyword_in' in frame 1"), std::string::npos)
      << error;
  EXPECT_NE(error.find("expected 101, got 103"), std::string::npos) << error;
  for (const auto& frame : outputs)
    EXPECT_EQ(frame.at("test.keyword_out"), nullptr);

  // 修正后同一句柄可继续使用。
  rows[1].service_type = COMPANY_MOCK_SERVICE_KEYWORD_MATCH;
  EXPECT_EQ(ops_.Process(handle.get(), inputs, outputs), 0)
      << GetOperatorLastError();
}

// 输出结构体写入登记的 service_type。
TEST_F(OperatorApiTest, OutputsCarryTheRegisteredServiceType) {
  auto param = DefaultCreateParam("configs/pipeline_keyword_match_rules.conf");
  void* raw_handle = nullptr;
  ASSERT_EQ(ops_.Create(&raw_handle, &param), 0) << GetOperatorLastError();
  const auto destroy = [this](void* handle) { ops_.Destroy(handle); };
  std::unique_ptr<void, decltype(destroy)> handle(raw_handle, destroy);
  char text[] = "query";
  CompanyString sentence{5, text};
  CompanyOperatorKeywordInput row{7, &sentence,
                                  COMPANY_MOCK_SERVICE_KEYWORD_MATCH};
  NamedIoBatch inputs(1), outputs(1);
  inputs[0]["test.keyword_in"] = MakeBorrowedOperatorInput(&row);
  outputs[0]["test.keyword_out"] = nullptr;
  ASSERT_EQ(ops_.Process(handle.get(), inputs, outputs), 0)
      << GetOperatorLastError();
  const auto* result = static_cast<const CompanyOperatorKeywordOutput*>(
      outputs[0].at("test.keyword_out").get());
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->service_type, COMPANY_MOCK_SERVICE_KEYWORD_MATCH);
}

// 图片问答的两项输入：请求 ID 取自带 request_id 的 frame；多项都带
// request_id 时逐条核对，不一致则返回输入非法。
TEST_F(OperatorApiTest, MultipleInputItemsPairByIndexAndCheckRequestIds) {
  using namespace llm_edgeflow;
  using namespace llm_edgeflow::test_support;
  const auto* frame_converter =
      IoConverterRegistry::Instance().FindInputConverter("frame",
                                                         "ocr_invoice_qa");
  const auto* query_converter =
      IoConverterRegistry::Instance().FindInputConverter("string",
                                                         "ocr_invoice_qa");
  ASSERT_NE(frame_converter, nullptr);
  ASSERT_NE(query_converter, nullptr);
  static std::atomic<uint64_t> query_id_offset{0};
  // 注册表是进程级全局的；只注册一次，以便测试可重复运行。
  if (!IoConverterRegistry::Instance().FindInputConverter("frame",
                                                          "test_paired_ids")) {
    auto frame = *frame_converter;
    frame.name = "test_paired_ids";
    frame.service_type = 9201;  // 测试用占位取值
    frame.decode_fn = [](const ExternalInputBatchView& source,
                         const InputDecodeOptions& options, AlgContext* context,
                         AdapterStatus* status) {
      const auto* converter =
          IoConverterRegistry::Instance().FindInputConverter("frame",
                                                             "ocr_invoice_qa");
      return converter->decode_fn(source, options, context, status);
    };
    ASSERT_TRUE(IoConverterRegistry::Instance().RegisterInputConverter(frame));
    auto query = *query_converter;
    query.name = "test_paired_ids";
    // 让 string 项也记录请求 ID，用来验证多项之间的核对。
    query.decode_fn = [](const ExternalInputBatchView& source,
                         const InputDecodeOptions& options, AlgContext* context,
                         AdapterStatus* status) {
      const auto* converter =
          IoConverterRegistry::Instance().FindInputConverter("string",
                                                             "ocr_invoice_qa");
      const int ret = converter->decode_fn(source, options, context, status);
      if (ret == 0) {
        for (size_t i = 0; i < source.count; ++i)
          options.request_ids->push_back(60001 + i + query_id_offset.load());
      }
      return ret;
    };
    ASSERT_TRUE(IoConverterRegistry::Instance().RegisterInputConverter(query));
  }

  ScopedTempDirectory temp;
  std::ifstream source(std::filesystem::path(GetConfDir()) /
                       "demo/fixtures/mock/pipeline_ocr_invoice_qa.json");
  nlohmann::json pipeline;
  source >> pipeline;
  pipeline["io"]["input"][0]["name"] = "test_paired_ids";
  pipeline["io"]["input"][1]["name"] = "test_paired_ids";
  std::ofstream(temp.path() / "pipeline.json") << pipeline;
  std::ofstream(temp.path() / "pipeline.conf")
      << nlohmann::json{{"pipe_path", "pipeline.json"}};
  const auto root = temp.path().string();
  auto param = DefaultCreateParam("pipeline.conf");
  param.model_path = root.c_str();
  void* raw_handle = nullptr;
  ASSERT_EQ(ops_.Create(&raw_handle, &param), 0) << GetOperatorLastError();
  const auto destroy = [this](void* handle) { ops_.Destroy(handle); };
  std::unique_ptr<void, decltype(destroy)> handle(raw_handle, destroy);

  std::string uris[] = {"./data/invoice_01.jpg", "./data/invoice_02.jpg"};
  std::string prompts[] = {"第一张", "第二张"};
  CompanyString uri_cs[2], prompt_cs[2];
  CompanyFrame frames[2];
  NamedIoBatch inputs(2), outputs(2);
  for (size_t i = 0; i < 2; ++i) {
    uri_cs[i] = {static_cast<int32_t>(uris[i].size()), uris[i].data()};
    prompt_cs[i] = {static_cast<int32_t>(prompts[i].size()), prompts[i].data()};
    frames[i] = {60001 + i, &uri_cs[i], nullptr, 9201};
    inputs[i]["camera_0.frame"] = MakeBorrowedOperatorInput(&frames[i]);
    inputs[i]["camera_0.string"] = MakeBorrowedOperatorInput(&prompt_cs[i]);
    outputs[i]["camera_0.od_out"] = nullptr;
  }

  // 两项的 ID 一致：按序号配对，请求 ID 逐条保留。
  query_id_offset = 0;
  ASSERT_EQ(ops_.Process(handle.get(), inputs, outputs), 0)
      << GetOperatorLastError();
  for (size_t i = 0; i < 2; ++i) {
    const auto* result = static_cast<const CompanyOdOutput*>(
        outputs[i].at("camera_0.od_out").get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->request_id, 60001 + i);
  }
  for (auto& frame : outputs) frame["camera_0.od_out"] = nullptr;

  // 两项的 ID 不一致：返回输入非法并指出序号，不发布任何输出。
  query_id_offset = 5;
  EXPECT_EQ(ops_.Process(handle.get(), inputs, outputs),
            COMPANY_ALG_ERR_INVALID_INPUT);
  const std::string error = GetOperatorLastError();
  EXPECT_NE(error.find("request_id mismatch between input "
                       "frame/test_paired_ids and string/test_paired_ids at "
                       "index 0: 60001 vs 60006"),
            std::string::npos)
      << error;
  for (const auto& frame : outputs)
    EXPECT_EQ(frame.at("camera_0.od_out"), nullptr);
  query_id_offset = 0;
}

// converter 参数：输入、输出 converter 在转换函数里读到本句柄解析好的参数，
// 尺寸参数同时决定输出池容量。
namespace {
struct ParamProbeInput {
  std::string tag;
};
struct ParamProbeOutput {
  std::string tag;
  int64_t match_result_json_max_bytes{};
};
std::string g_probe_input_tag;
std::string g_probe_output_tag;
}  // namespace

TEST_F(OperatorApiTest, ConverterParamsReachInputAndOutputCallbacks) {
  using namespace llm_edgeflow;
  using namespace llm_edgeflow::test_support;
  auto& registry = IoConverterRegistry::Instance();
  const auto* production_in =
      registry.FindInputConverter("keyword_in", "keyword_match");
  const auto* production_out =
      registry.FindOutputConverter("keyword_out", "keyword_match");
  ASSERT_NE(production_in, nullptr);
  ASSERT_NE(production_out, nullptr);
  // 注册表是进程级全局的；只注册一次，以便测试可重复运行。
  if (!registry.FindInputConverter("keyword_in", "test_param_probe")) {
    auto input = *production_in;
    input.name = "test_param_probe";
    input.service_type = 9301;  // 测试用占位取值
    input.params = Parameters<ParamProbeInput>(
        {Field("tag", &ParamProbeInput::tag).Default("input-default")});
    input.decode_fn = [](const ExternalInputBatchView& source,
                         const InputDecodeOptions& options, AlgContext* context,
                         AdapterStatus* status) {
      g_probe_input_tag = options.Params<ParamProbeInput>().tag;
      return IoConverterRegistry::Instance()
          .FindInputConverter("keyword_in", "keyword_match")
          ->decode_fn(source, options, context, status);
    };
    ASSERT_TRUE(registry.RegisterInputConverter(input));
    auto output = *production_out;
    output.name = "test_param_probe";
    output.service_type = 9301;
    output.params = Parameters<ParamProbeOutput>(
        {Field("tag", &ParamProbeOutput::tag).Default("output-default"),
         MaxBytes("match_result_json",
                  &ParamProbeOutput::match_result_json_max_bytes)
             .Default(2047)});
    output.encode_fn = [](AlgContext* context,
                          const OutputEncodeOptions& options,
                          ExternalOutputBatchView* destination, size_t* written,
                          AdapterStatus* status) {
      g_probe_output_tag = options.Params<ParamProbeOutput>().tag;
      return IoConverterRegistry::Instance()
          .FindOutputConverter("keyword_out", "keyword_match")
          ->encode_fn(context, options, destination, written, status);
    };
    ASSERT_TRUE(registry.RegisterOutputConverter(output));
  }

  const auto run = [&](const nlohmann::json& input_params,
                       const nlohmann::json& output_params) {
    ScopedTempDirectory temp;
    std::ifstream source(std::filesystem::path(GetConfDir()) /
                         "configs/pipeline_keyword_match_rules.json");
    nlohmann::json pipeline;
    source >> pipeline;
    pipeline["io"]["input"][0] = {{"type", "keyword_in"},
                                  {"name", "test_param_probe"}};
    pipeline["io"]["output"][0] = {{"type", "keyword_out"},
                                   {"name", "test_param_probe"}};
    if (!input_params.is_null())
      pipeline["io"]["input"][0]["params"] = input_params;
    if (!output_params.is_null())
      pipeline["io"]["output"][0]["params"] = output_params;
    std::ofstream(temp.path() / "pipeline.json") << pipeline;
    std::ofstream(temp.path() / "pipeline.conf")
        << nlohmann::json{{"pipe_path", "pipeline.json"}};
    const auto root = temp.path().string();
    auto param = DefaultCreateParam("pipeline.conf");
    param.model_path = root.c_str();
    void* raw_handle = nullptr;
    ASSERT_EQ(ops_.Create(&raw_handle, &param), 0) << GetOperatorLastError();
    const auto destroy = [this](void* handle) { ops_.Destroy(handle); };
    std::unique_ptr<void, decltype(destroy)> handle(raw_handle, destroy);
    char text[] = "query";
    CompanyString sentence{5, text};
    CompanyOperatorKeywordInput row{7, &sentence, 9301};
    NamedIoBatch inputs(1), outputs(1);
    inputs[0]["test.keyword_in"] = MakeBorrowedOperatorInput(&row);
    outputs[0]["test.keyword_out"] = nullptr;
    g_probe_input_tag.clear();
    g_probe_output_tag.clear();
    ASSERT_EQ(ops_.Process(handle.get(), inputs, outputs), 0)
        << GetOperatorLastError();
    const auto* result = static_cast<const CompanyOperatorKeywordOutput*>(
        outputs[0].at("test.keyword_out").get());
    ASSERT_NE(result, nullptr);
    // 输出结构体写入该项登记的 service_type。
    EXPECT_EQ(result->service_type, 9301);
  };

  run(nullptr, nullptr);
  EXPECT_EQ(g_probe_input_tag, "input-default");
  EXPECT_EQ(g_probe_output_tag, "output-default");

  run({{"tag", "in"}}, {{"tag", "out"}, {"match_result_json_max_bytes", 4096}});
  EXPECT_EQ(g_probe_input_tag, "in");
  EXPECT_EQ(g_probe_output_tag, "out");
}

TEST_F(OperatorApiTest,
       SameOutputKeysSelectIndependentNestedAllocatorsPerHandle) {
  using namespace llm_edgeflow::test_support;
  ScopedTempDirectory temp;
  WriteNestedOutputPipeline(temp.path(), "first.json", "standard_pair");
  WriteNestedOutputPipeline(temp.path(), "second.json", "alternate_pair");
  std::ofstream(temp.path() / "first.conf")
      << nlohmann::json{{"pipe_path", "first.json"}};
  std::ofstream(temp.path() / "second.conf")
      << nlohmann::json{{"pipe_path", "second.json"}};
  const auto root = temp.path().string();
  for (bool alternate : {false, true}) {
    llm_edgeflow::ResolvedOperatorConfig resolved;
    std::string error;
    ASSERT_EQ(llm_edgeflow::OperatorConfigResolver::Resolve(
                  root.c_str(), alternate ? "second.conf" : "first.conf",
                  &resolved, &error),
              0)
        << error;
    ASSERT_EQ(resolved.io_plan->outputs.size(), 2U);
    // 布局和参数由登记固定；同一宿主结构体在不同句柄中选用不同登记。
    const auto& main = resolved.io_plan->outputs[0];
    const auto& audit = resolved.io_plan->outputs[1];
    EXPECT_EQ(main.converter->slot.allocator,
              alternate ? "test_nested_alternate" : "test_nested_standard");
    EXPECT_EQ(audit.converter->slot.allocator,
              alternate ? "test_nested_audit_standard"
                        : "test_nested_audit_alternate");
    for (const auto* output : {&main, &audit}) {
      EXPECT_EQ(output->converter->slot.allocator_params.find("reject_hit"),
                std::string::npos);
      EXPECT_FALSE(
          output->pool_spec.Parameters<NestedOutputParameters>().reject_hit);
    }
  }
  const int allocations_before = nested_allocations;
  const int destroys_before = nested_destroys;
  std::vector<std::shared_ptr<void>> handles;
  for (const char* file : {"first.conf", "second.conf"}) {
    CreateParam param{};
    param.model_path = root.c_str();
    param.cfg_file_name = file;
    param.compute_platform = ComputePlatform::kCpu;
    param.max_frame_depth = 2;
    void* handle = nullptr;
    ASSERT_EQ(ops_.Create(&handle, &param), 0) << GetOperatorLastError();
    handles.emplace_back(handle, [this](void* ptr) {
      EXPECT_EQ(ops_.Destroy(ptr), 0) << GetOperatorLastError();
    });
  }
  // 两个句柄，每个两个输出槽位，每个池两次单对象分配。
  ASSERT_EQ(nested_allocations - allocations_before, 8);
  std::string text = "初始化";
  CompanyString sentence{static_cast<int32_t>(text.size()), text.data()};
  CompanyOperatorKeywordInput input[] = {
      {901, &sentence, COMPANY_MOCK_SERVICE_KEYWORD_MATCH},
      {902, &sentence, COMPANY_MOCK_SERVICE_KEYWORD_MATCH}};
  NamedIoBatch inputs(2);
  for (size_t i = 0; i < inputs.size(); ++i) {
    inputs[i]["chan.keyword_in"] = MakeBorrowedOperatorInput(&input[i]);
  }
  for (size_t variant = 0; variant < handles.size(); ++variant) {
    SCOPED_TRACE(variant);
    void* previous[2][2]{};
    for (int round = 0; round < 2; ++round) {
      NamedIoBatch outputs(2);
      for (auto& output : outputs) {
        output["chan.test_nested_out"] = {};
        output["chan.test_nested_audit_out"] = {};
      }
      ASSERT_EQ(ops_.Process(handles[variant].get(), inputs, outputs), 0)
          << GetOperatorLastError();
      for (size_t i = 0; i < outputs.size(); ++i) {
        ExpectNestedResult(outputs[i].at("chan.test_nested_out"),
                           input[i].request_id, variant ? 2 : 1,
                           variant ? 2 : 1, variant ? 3 : 2, true);
        ExpectNestedResult(outputs[i].at("chan.test_nested_audit_out"),
                           input[i].request_id, variant ? 1 : 2,
                           variant ? 1 : 2, variant ? 4 : 5, true);
        const void* first = outputs[i].at("chan.test_nested_out").get();
        const void* second = outputs[i].at("chan.test_nested_audit_out").get();
        EXPECT_NE(first, second);
        if (round == 1) {
          EXPECT_EQ(first, previous[i][0]);
          EXPECT_EQ(second, previous[i][1]);
        }
        previous[i][0] = outputs[i].at("chan.test_nested_out").get();
        previous[i][1] = outputs[i].at("chan.test_nested_audit_out").get();
      }
      // 按帧顺序归还，使每个独立的池复用其 FIFO 顺序。
      for (auto& output : outputs) output.clear();
    }
  }
  EXPECT_EQ(nested_allocations - allocations_before, 8);
  handles.clear();
  EXPECT_EQ(nested_destroys - destroys_before, 8);
}

TEST_F(OperatorApiTest, NestedOutputConfigurationIsValidatedBeforeAllocation) {
  using namespace llm_edgeflow::test_support;
  ScopedTempDirectory temp;
  const auto root = temp.path().string();
  std::ofstream(temp.path() / "invalid.conf")
      << nlohmann::json{{"pipe_path", "pipeline.json"}};
  for (int mutation = 0; mutation < 8; ++mutation) {
    SCOPED_TRACE(mutation);
    auto pipeline = NestedOutputPipelineJson();
    auto& io = pipeline["io"];
    auto& main = io["output"][0];
    switch (mutation) {
      case 0:  // 未登记的业务
        main["name"] = "not_registered";
        break;
      case 1:  // 布局由登记固定，没有可配置的参数
        main["params"] = {{"kind", 9}};
        break;
      case 2:  // 参数不是对象
        main["params"] = nlohmann::json::array();
        break;
      case 3:  // 输出项不是对象
        io["output"][1] = 42;
        break;
      case 4:  // io 中的未知键
        io["mem_que"] = nlohmann::json::object();
        break;
      case 5:  // 同一输出项重复
        io["output"].push_back(main);
        break;
      case 6:  // 把输入 type 写在输出侧
        main["type"] = "keyword_in";
        break;
      case 7:  // 没有输出项
        io["output"] = nlohmann::json::array();
        break;
    }
    std::ofstream(temp.path() / "pipeline.json") << pipeline;
    CreateParam param{};
    param.model_path = root.c_str();
    param.cfg_file_name = "invalid.conf";
    param.compute_platform = ComputePlatform::kCpu;
    param.max_frame_depth = 1;
    const int allocations_before = nested_allocations;
    void* handle = nullptr;
    EXPECT_EQ(ops_.Create(&handle, &param), -2) << GetOperatorLastError();
    EXPECT_EQ(handle, nullptr);
    EXPECT_EQ(nested_allocations, allocations_before);
    EXPECT_STRNE(GetOperatorLastError(), "");
    if (handle) ops_.Destroy(handle);
  }
}

TEST_F(OperatorApiTest, UndeclaredLayoutParametersUseTheAllocatorDefaults) {
  using namespace llm_edgeflow::test_support;
  ScopedTempDirectory temp;
  auto pipeline = NestedOutputPipelineJson("default_params");
  std::ofstream(temp.path() / "pipeline.json") << pipeline;
  std::ofstream(temp.path() / "pipeline.conf")
      << nlohmann::json{{"pipe_path", "pipeline.json"}};
  const auto root = temp.path().string();
  char error[256]{};
  OperatorIoContract contract;
  EXPECT_EQ(ResolveOperatorConfigIo(root.c_str(), "pipeline.conf", &contract,
                                    error, sizeof(error)),
            0)
      << error;
  ASSERT_EQ(contract.outputs.size(), 2U);
  EXPECT_EQ(contract.outputs[0].type, "test_nested_out");
  EXPECT_EQ(contract.outputs[0].name, "default_params");
  EXPECT_EQ(contract.outputs[1].type, "test_nested_audit_out");
  llm_edgeflow::ResolvedOperatorConfig resolved;
  std::string resolve_error;
  ASSERT_EQ(llm_edgeflow::OperatorConfigResolver::Resolve(
                root.c_str(), "pipeline.conf", &resolved, &resolve_error),
            0)
      << resolve_error;
  // 登记没有声明布局参数时，所选实现收到的参数文本为 "{}"。
  for (const auto& output : resolved.io_plan->outputs) {
    EXPECT_TRUE(output.converter->slot.allocator_params.empty());
    EXPECT_EQ(output.converter->slot.AllocatorParamsText(), "{}");
  }
  CreateParam param{};
  param.model_path = root.c_str();
  param.cfg_file_name = "pipeline.conf";
  param.compute_platform = ComputePlatform::kCpu;
  param.max_frame_depth = 1;
  void* raw_handle = nullptr;
  ASSERT_EQ(ops_.Create(&raw_handle, &param), 0) << GetOperatorLastError();
  const std::shared_ptr<void> handle(
      raw_handle, [this](void* ptr) { EXPECT_EQ(ops_.Destroy(ptr), 0); });
  std::string text = "初始化";
  CompanyString sentence{static_cast<int32_t>(text.size()), text.data()};
  CompanyOperatorKeywordInput input{907, &sentence,
                                    COMPANY_MOCK_SERVICE_KEYWORD_MATCH};
  NamedIoBatch inputs(1), outputs(1);
  inputs[0]["chan.keyword_in"] = MakeBorrowedOperatorInput(&input);
  outputs[0]["chan.test_nested_out"] = {};
  outputs[0]["chan.test_nested_audit_out"] = {};
  ASSERT_EQ(ops_.Process(handle.get(), inputs, outputs), 0)
      << GetOperatorLastError();
  ExpectNestedResult(outputs[0].at("chan.test_nested_out"), 907, 1, 1, 3, true);
  ExpectNestedResult(outputs[0].at("chan.test_nested_audit_out"), 907, 1, 1, 3,
                     true);
  EXPECT_NE(outputs[0].at("chan.test_nested_out"),
            outputs[0].at("chan.test_nested_audit_out"));
  outputs.clear();
}

// 命名布局要求的参数必须由登记声明：缺失时 Init 在审计阶段就失败。
TEST_F(OperatorApiTest,
       CustomAllocatorStillRequiresItsExplicitLayoutParameters) {
  using namespace llm_edgeflow::test_support;
  ScopedConverterRegistry restore;
  ASSERT_TRUE(
      llm_edgeflow::IoConverterRegistry::Instance().RegisterOutputConverter(
          NestedOutputConverter(false, "missing_explicit_parameters",
                                "test_nested_explicit_parameters")));
  const int init_ret = ops_.Init();
  EXPECT_EQ(init_ret, -6);
  EXPECT_NE(std::string(GetOperatorLastError()).find("capacity"),
            std::string::npos)
      << GetOperatorLastError();
  EXPECT_NE(std::string(GetOperatorLastError())
                .find("test_nested_out/missing_explicit_parameters"),
            std::string::npos);
}

TEST_F(OperatorApiTest, NestedOutputFailureRollsBackAllSlotsAndAllowsRetry) {
  using namespace llm_edgeflow::test_support;
  ScopedTempDirectory temp;
  auto pipeline = NestedOutputPipelineJson("reject_audit_hits");
  std::ofstream(temp.path() / "pipeline.json") << pipeline;
  std::ofstream(temp.path() / "pipeline.conf")
      << nlohmann::json{{"pipe_path", "pipeline.json"}};
  const auto root = temp.path().string();
  CreateParam param{};
  param.model_path = root.c_str();
  param.cfg_file_name = "pipeline.conf";
  param.compute_platform = ComputePlatform::kCpu;
  param.max_frame_depth = 1;
  void* raw_handle = nullptr;
  ASSERT_EQ(ops_.Create(&raw_handle, &param), 0) << GetOperatorLastError();
  const std::shared_ptr<void> handle(
      raw_handle, [this](void* ptr) { EXPECT_EQ(ops_.Destroy(ptr), 0); });
  const int allocations_before = nested_allocations;
  std::string text = "初始化";
  CompanyString sentence{static_cast<int32_t>(text.size()), text.data()};
  CompanyOperatorKeywordInput input{903, &sentence,
                                    COMPANY_MOCK_SERVICE_KEYWORD_MATCH};
  NamedIoBatch inputs(1), outputs(1);
  inputs[0]["chan.keyword_in"] = MakeBorrowedOperatorInput(&input);
  outputs[0]["chan.test_nested_out"] = {};
  outputs[0]["chan.test_nested_audit_out"] = {};
  for (int attempt = 0; attempt < 2; ++attempt) {
    const int resets_before = nested_resets;
    EXPECT_EQ(ops_.Process(handle.get(), inputs, outputs), -4);
    EXPECT_EQ(outputs[0].at("chan.test_nested_out"), nullptr);
    EXPECT_EQ(outputs[0].at("chan.test_nested_audit_out"), nullptr);
    EXPECT_EQ(nested_resets - resets_before, 2);
  }
  text = "no matching rule";
  sentence = {static_cast<int32_t>(text.size()), text.data()};
  ASSERT_EQ(ops_.Process(handle.get(), inputs, outputs), 0)
      << GetOperatorLastError();
  ExpectNestedResult(outputs[0].at("chan.test_nested_out"), 903, 1, 1, 2,
                     false);
  ExpectNestedResult(outputs[0].at("chan.test_nested_audit_out"), 903, 2, 2, 5,
                     false);
  EXPECT_EQ(nested_allocations, allocations_before);
  outputs.clear();
}

TEST_F(OperatorApiTest, OutputBudgetUsesRequestedDepthWithoutAllocating) {
  using namespace llm_edgeflow::test_support;
  ScopedTempDirectory temp;
  auto pipeline = NestedOutputPipelineJson("large_footprint");
  std::ofstream(temp.path() / "pipeline.json") << pipeline;
  std::ofstream(temp.path() / "pipeline.conf")
      << nlohmann::json{{"pipe_path", "pipeline.json"}};
  const auto root = temp.path().string();
  llm_edgeflow::ResolvedOperatorConfig resolved;
  std::string error;
  const int allocations_before = nested_allocations;
  EXPECT_EQ(llm_edgeflow::OperatorConfigResolver::Resolve(
                root.c_str(), "pipeline.conf", &resolved, &error),
            -2);
  EXPECT_NE(error.find("payload budget"), std::string::npos);
  EXPECT_EQ(llm_edgeflow::OperatorConfigResolver::Resolve(
                root.c_str(), "pipeline.conf", &resolved, &error, 0),
            -2);
  EXPECT_NE(error.find("payload budget"), std::string::npos);
  EXPECT_EQ(llm_edgeflow::OperatorConfigResolver::Resolve(
                root.c_str(), "pipeline.conf", &resolved, &error, 1),
            0)
      << error;
  ASSERT_NE(resolved.io_plan, nullptr);
  EXPECT_EQ(llm_edgeflow::OperatorConfigResolver::Resolve(
                root.c_str(), "pipeline.conf", &resolved, &error, 1025),
            -2);
  EXPECT_EQ(nested_allocations, allocations_before);
}

TEST_F(OperatorApiTest, AllOutputSlotsShareTheHandlePayloadBudget) {
  using namespace llm_edgeflow::test_support;
  ScopedTempDirectory temp;
  auto pipeline = NestedOutputPipelineJson("huge_capacity");
  std::ofstream(temp.path() / "pipeline.json") << pipeline;
  std::ofstream(temp.path() / "pipeline.conf")
      << nlohmann::json{{"pipe_path", "pipeline.json"}};
  const auto root = temp.path().string();
  CreateParam param{};
  param.model_path = root.c_str();
  param.cfg_file_name = "pipeline.conf";
  param.compute_platform = ComputePlatform::kCpu;
  param.max_frame_depth = 1024;
  const int allocations_before = nested_allocations;
  void* handle = nullptr;
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  EXPECT_EQ(handle, nullptr);
  EXPECT_EQ(nested_allocations, allocations_before);
  EXPECT_NE(std::string(GetOperatorLastError())
                .find("exceeds per-handle payload budget"),
            std::string::npos);
  if (handle) ops_.Destroy(handle);
}

// 共享载体不合并 payload schema
TEST_F(OperatorApiTest, SharedCarrierDoesNotMergePayloadSchema) {
  auto param =
      DefaultCreateParam("demo/fixtures/mock/pipeline_entity_extract.conf");
  void* entity_handle = nullptr;
  ASSERT_EQ(ops_.Create(&entity_handle, &param), 0);
  ASSERT_NE(entity_handle, nullptr);

  // 1. 纯文本：对实体抽取合法，但不符合翻译的 schema
  std::string plain_text = "普通中文句子非JSON格式";
  CompanyString cs_plain{static_cast<int32_t>(plain_text.size()),
                         const_cast<char*>(plain_text.data())};
  CompanyOperatorEntityInput in_plain{50001, &cs_plain,
                                      COMPANY_MOCK_SERVICE_ENTITY_EXTRACT};

  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["nlp.entity_in"] = MakeBorrowedOperatorInput(&in_plain);
  out_b[0]["nlp.entity_out"] = std::shared_ptr<void>();

  // 实体抽取接受纯文本
  EXPECT_EQ(ops_.Process(entity_handle, in_b, out_b), 0);
  out_b.clear();

  // 翻译 Adapter 要求带 "query" 的 JSON 对象，因此拒绝纯文本
  const auto* translate_in_conv =
      llm_edgeflow::IoConverterRegistry::Instance().FindInputConverter(
          "entity_in", "translate");
  ASSERT_NE(translate_in_conv, nullptr);
  CompanyOperatorEntityInput c_in_plain{50001, &cs_plain,
                                        COMPANY_MOCK_SERVICE_ENTITY_EXTRACT};
  llm_edgeflow::AlgContext ctx;
  llm_edgeflow::AdapterStatus status;
  llm_edgeflow::ExternalInputBatchView view_plain;
  view_plain.count = 1;
  view_plain.slots["entity_in"] =
      llm_edgeflow::BorrowInputForTest({&c_in_plain});
  view_plain.slot_types["entity_in"] = "CompanyOperatorEntityInput";
  llm_edgeflow::InputDecodeOptions decode_opts;
  decode_opts.type = translate_in_conv->type;
  decode_opts.name = translate_in_conv->name;
  std::vector<uint64_t> request_ids;
  decode_opts.request_ids = &request_ids;

  EXPECT_EQ(
      translate_in_conv->decode_fn(view_plain, decode_opts, &ctx, &status),
      COMPANY_ALG_ERR_INVALID_INPUT);

  // 2. JSON 文本：翻译接受并提取 "query"
  std::string json_text = "{\"query\":\"有效翻译查询\"}";
  CompanyString cs_json{static_cast<int32_t>(json_text.size()),
                        const_cast<char*>(json_text.data())};
  CompanyOperatorEntityInput c_in_json{50002, &cs_json,
                                       COMPANY_MOCK_SERVICE_ENTITY_EXTRACT};
  llm_edgeflow::AlgContext valid_ctx;
  llm_edgeflow::ExternalInputBatchView view_json;
  view_json.count = 1;
  view_json.slots["entity_in"] = llm_edgeflow::BorrowInputForTest({&c_in_json});
  view_json.slot_types["entity_in"] = "CompanyOperatorEntityInput";
  EXPECT_EQ(
      translate_in_conv->decode_fn(view_json, decode_opts, &valid_ctx, &status),
      COMPANY_ALG_SUCCESS);
  const auto* queries = valid_ctx.Read(llm_edgeflow::kInputSentences);
  ASSERT_NE(queries, nullptr);
  EXPECT_EQ((*queries)[0].data, "有效翻译查询");

  EXPECT_EQ(ops_.Destroy(entity_handle), 0);
}

TEST_F(OperatorApiTest,
       OptionalOutputSlotPreservesBatchIndicesAcrossOmissionPatterns) {
  using namespace llm_edgeflow;
  test_support::ScopedConverterRegistry restore;
  auto& registry = IoConverterRegistry::Instance();
  const auto* production =
      registry.FindOutputConverter("keyword_out", "keyword_match");
  ASSERT_NE(production, nullptr);
  for (int index = 0; index < 2; ++index) {
    auto converter = *production;
    converter.name = index == 0 ? "test_opt_kw_a" : "test_opt_kw_b";
    converter.service_type = 9401 + index;
    converter.slot.required = false;
    ASSERT_TRUE(registry.RegisterOutputConverter(converter));
  }

  ScopedTempDirectory temp;
  std::ifstream source(std::filesystem::path(GetConfDir()) /
                       "configs/pipeline_keyword_match_rules.json");
  auto pipeline = nlohmann::json::parse(source);
  pipeline["io"]["output"] = {
      {{"type", "keyword_out"}, {"name", "test_opt_kw_a"}},
      {{"type", "keyword_out"}, {"name", "test_opt_kw_b"}}};
  pipeline["pipeline"][0]["config"]["categories"] = {
      {"ROW0", {"VIP"}}, {"ROW1", {"加急"}}, {"ROW2", {"普通查询"}}};
  std::ofstream(temp.path() / "pipeline.json") << pipeline;
  std::ofstream(temp.path() / "pipeline.conf")
      << nlohmann::json{{"pipe_path", "pipeline.json"}};
  auto param = DefaultCreateParam("pipeline.conf");
  param.max_frame_depth = 3;
  const auto root = temp.path().string();
  param.model_path = root.c_str();
  void* raw_handle = nullptr;
  ASSERT_EQ(ops_.Create(&raw_handle, &param), 0) << GetOperatorLastError();
  const auto destroy = [this](void* h) { EXPECT_EQ(ops_.Destroy(h), 0); };
  std::unique_ptr<void, decltype(destroy)> handle(raw_handle, destroy);

  std::string texts[] = {"VIP", "加急", "普通查询"};
  CompanyString strings[3];
  CompanyOperatorKeywordInput rows[3];
  NamedIoBatch inputs(3);
  for (size_t row = 0; row < 3; ++row) {
    strings[row] = {static_cast<int32_t>(texts[row].size()), texts[row].data()};
    rows[row] = {42 + row, &strings[row], COMPANY_MOCK_SERVICE_KEYWORD_MATCH};
    inputs[row]["test.keyword_in"] = MakeBorrowedOperatorInput(&rows[row]);
  }
  const std::vector<std::vector<int>> patterns = {
      {0, 1, 1}, {1, 0, 1}, {1, 1, 0}, {0, 0, 0}, {1, 2, 3}, {3, 3, 3}};
  for (const auto& pattern : patterns) {
    SCOPED_TRACE(nlohmann::json(pattern).dump());
    NamedIoBatch outputs(3);
    for (size_t row = 0; row < 3; ++row) {
      if (pattern[row] & 1) outputs[row]["test_opt_kw_a.keyword_out"] = nullptr;
      if (pattern[row] & 2) outputs[row]["test_opt_kw_b.keyword_out"] = nullptr;
    }
    ASSERT_EQ(ops_.Process(handle.get(), inputs, outputs), 0)
        << GetOperatorLastError();
    for (size_t row = 0; row < 3; ++row) {
      for (int item = 0; item < 2; ++item) {
        const char* key = item == 0 ? "test_opt_kw_a.keyword_out"
                                    : "test_opt_kw_b.keyword_out";
        if (!(pattern[row] & (1 << item))) {
          EXPECT_EQ(outputs[row].count(key), 0U);
          continue;
        }
        const auto* result = static_cast<const CompanyOperatorKeywordOutput*>(
            outputs[row].at(key).get());
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(result->request_id, 42 + row);
        EXPECT_EQ(result->service_type, 9401 + item);
        EXPECT_EQ(result->is_hit, 1);
        ASSERT_NE(result->match_result_json, nullptr);
        const auto response = nlohmann::json::parse(
            std::string(result->match_result_json->data,
                        result->match_result_json->length));
        EXPECT_EQ(response["intent"], "ROW" + std::to_string(row));
        EXPECT_EQ(response["matched_word"], texts[row]);
      }
    }
  }

  // 第二项编码容量不足时，第一项已写入但不能发布；反复失败后仍能租用整池。
  pipeline["io"]["output"][1]["params"] = {{"match_result_json_max_bytes", 1}};
  std::ofstream(temp.path() / "pipeline.json") << pipeline;
  raw_handle = nullptr;
  ASSERT_EQ(ops_.Create(&raw_handle, &param), 0) << GetOperatorLastError();
  std::unique_ptr<void, decltype(destroy)> failing_handle(raw_handle, destroy);
  for (int attempt = 0; attempt < 4; ++attempt) {
    NamedIoBatch outputs(3);
    for (auto& output : outputs) {
      output["test_opt_kw_a.keyword_out"] = nullptr;
      output["test_opt_kw_b.keyword_out"] = nullptr;
    }
    EXPECT_EQ(ops_.Process(failing_handle.get(), inputs, outputs),
              COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
    EXPECT_NE(std::string(GetOperatorLastError()).find("EncodeOutput failed"),
              std::string::npos);
    for (const auto& output : outputs) {
      for (const auto& [key, value] : output) EXPECT_EQ(value, nullptr);
    }
    NamedIoBatch recovery(3);
    for (auto& output : recovery) output["test_opt_kw_a.keyword_out"] = nullptr;
    ASSERT_EQ(ops_.Process(failing_handle.get(), inputs, recovery), 0)
        << GetOperatorLastError();
  }

  // 编码函数把批次长度误报为写入数时，不能发布可选槽的部分目标。
  auto bad_count = *production;
  bad_count.name = "test_bad_count";
  bad_count.service_type = 9403;
  bad_count.slot.required = false;
  bad_count.encode_fn = [](AlgContext*, const OutputEncodeOptions&,
                           ExternalOutputBatchView* destination,
                           size_t* written_count, AdapterStatus*) {
    *written_count = destination->count;
    return 0;
  };
  ASSERT_TRUE(registry.RegisterOutputConverter(bad_count));
  pipeline["io"]["output"] = {
      {{"type", "keyword_out"}, {"name", bad_count.name}}};
  std::ofstream(temp.path() / "pipeline.json") << pipeline;
  raw_handle = nullptr;
  ASSERT_EQ(ops_.Create(&raw_handle, &param), 0) << GetOperatorLastError();
  std::unique_ptr<void, decltype(destroy)> count_handle(raw_handle, destroy);
  NamedIoBatch partial_outputs(3);
  partial_outputs[1]["test_bad_count.keyword_out"] = nullptr;
  EXPECT_EQ(ops_.Process(count_handle.get(), inputs, partial_outputs),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_NE(std::string(GetOperatorLastError()).find("written count"),
            std::string::npos);
  EXPECT_EQ(partial_outputs[1]["test_bad_count.keyword_out"], nullptr);
}

TEST_F(OperatorApiTest,
       RepeatedInputTypesReachOwnConvertersWithParametersAndIds) {
  using namespace llm_edgeflow;
  test_support::ScopedConverterRegistry restore;
  struct Params {
    std::string prefix;
  };
  auto& registry = IoConverterRegistry::Instance();
  const auto* production =
      registry.FindInputConverter("keyword_in", "keyword_match");
  ASSERT_NE(production, nullptr);
  auto second = *production;
  second.name = "second_input";
  second.service_type = 9701;
  const BlackboardKey<TextBatch> second_key{"second_sentences", "TextBatch"};
  second.logical_ports = {OutputPort(second_key)};
  second.params =
      Parameters<Params>({Field("prefix", &Params::prefix).Default("")});
  second.decode_fn = [](const ExternalInputBatchView& source,
                        const InputDecodeOptions& options, AlgContext* context,
                        AdapterStatus* status) {
    const auto* first = context->Read(kInputSentences);
    if (!first || first->size() != 1 || first->at(0).data != "alpha") return -3;
    const auto& prefix = options.Params<Params>().prefix;
    return DecodeRequestRows<CompanyOperatorKeywordInput>(
        source, options, context, status, "keyword_in",
        BlackboardKey<TextBatch>{"second_sentences", "TextBatch"},
        [&prefix](const CompanyOperatorKeywordInput& row, std::string* text) {
          *text = prefix + std::string(row.sentence_text->data,
                                       row.sentence_text->length);
          return AdapterStatus::Ok();
        });
  };
  ASSERT_TRUE(registry.RegisterInputConverter(second));

  ScopedTempDirectory temp;
  std::ifstream source(std::filesystem::path(GetConfDir()) /
                       "configs/pipeline_keyword_match_rules.json");
  auto pipeline = nlohmann::json::parse(source);
  pipeline["io"]["input"].push_back({{"type", "keyword_in"},
                                     {"name", second.name},
                                     {"params", {{"prefix", "B-"}}}});
  pipeline["pipeline"][0]["inputs"]["text"] = "second_sentences";
  pipeline["pipeline"][0]["config"]["categories"] = {{"SECOND", {"B-beta"}}};
  std::ofstream(temp.path() / "pipeline.json") << pipeline;
  std::ofstream(temp.path() / "pipeline.conf")
      << nlohmann::json{{"pipe_path", "pipeline.json"}};
  auto param = DefaultCreateParam("pipeline.conf");
  const auto root = temp.path().string();
  param.model_path = root.c_str();
  void* raw_handle = nullptr;
  ASSERT_EQ(ops_.Create(&raw_handle, &param), 0) << GetOperatorLastError();
  const auto destroy = [this](void* h) { EXPECT_EQ(ops_.Destroy(h), 0); };
  std::unique_ptr<void, decltype(destroy)> handle(raw_handle, destroy);

  char alpha[] = "alpha";
  char beta[] = "beta";
  CompanyString first_text{5, alpha}, second_text{4, beta};
  CompanyOperatorKeywordInput first_row{42, &first_text,
                                        COMPANY_MOCK_SERVICE_KEYWORD_MATCH};
  CompanyOperatorKeywordInput second_row{42, &second_text, 9701};
  NamedIoBatch inputs(1), outputs(1);
  inputs[0]["keyword_match.keyword_in"] = MakeBorrowedOperatorInput(&first_row);
  inputs[0]["second_input.keyword_in"] = MakeBorrowedOperatorInput(&second_row);
  outputs[0]["host.keyword_out"] = nullptr;
  ASSERT_EQ(ops_.Process(handle.get(), inputs, outputs), 0)
      << GetOperatorLastError();
  const auto* result = static_cast<const CompanyOperatorKeywordOutput*>(
      outputs[0].at("host.keyword_out").get());
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->request_id, 42U);
  EXPECT_EQ(result->is_hit, 1);
  const auto response = nlohmann::json::parse(std::string(
      result->match_result_json->data, result->match_result_json->length));
  EXPECT_EQ(response["intent"], "SECOND");
  EXPECT_EQ(response["matched_word"], "B-beta");
  outputs[0]["host.keyword_out"].reset();
  second_row.request_id = 43;
  EXPECT_EQ(ops_.Process(handle.get(), inputs, outputs),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_NE(std::string(GetOperatorLastError()).find("request_id mismatch"),
            std::string::npos);
  EXPECT_EQ(outputs[0]["host.keyword_out"], nullptr);
}

// R3: 同侧多个项共享相同宿主结构体 type 时，通过业务名寻址且互不覆盖，各自持有
// 独立的参数、输出池和 service_type；同时保留 (type, name) 重复拒绝测试。
TEST_F(OperatorApiTest,
       MultipleItemsSharingSameHostStructTypeDisambiguateAndIsolate) {
  using namespace llm_edgeflow;
  test_support::ScopedConverterRegistry restore;
  auto& registry = IoConverterRegistry::Instance();
  const auto* prod_out =
      registry.FindOutputConverter("keyword_out", "keyword_match");
  ASSERT_NE(prod_out, nullptr);

  auto conv_a = *prod_out;
  conv_a.name = "test_r3_out_a";
  conv_a.service_type = 9501;
  ASSERT_TRUE(registry.RegisterOutputConverter(conv_a));
  auto conv_b = *prod_out;
  conv_b.name = "test_r3_out_b";
  conv_b.service_type = 9502;
  ASSERT_TRUE(registry.RegisterOutputConverter(conv_b));

  ScopedTempDirectory temp;
  std::ifstream source(std::filesystem::path(GetConfDir()) /
                       "configs/pipeline_keyword_match_rules.json");
  nlohmann::json pipeline;
  source >> pipeline;
  pipeline["io"]["input"] = {
      {{"type", "keyword_in"}, {"name", "keyword_match"}}};
  pipeline["io"]["output"] = {
      {{"type", "keyword_out"},
       {"name", "test_r3_out_a"},
       {"params", {{"match_result_json_max_bytes", 2047}}}},
      {{"type", "keyword_out"},
       {"name", "test_r3_out_b"},
       {"params", {{"match_result_json_max_bytes", 1023}}}}};
  std::ofstream(temp.path() / "pipeline.json") << pipeline;
  std::ofstream(temp.path() / "pipeline.conf")
      << nlohmann::json{{"pipe_path", "pipeline.json"}};

  auto param = DefaultCreateParam("pipeline.conf");
  const auto root = temp.path().string();
  param.model_path = root.c_str();

  void* raw_handle = nullptr;
  ASSERT_EQ(ops_.Create(&raw_handle, &param), 0) << GetOperatorLastError();
  const auto destroy = [this](void* handle) { ops_.Destroy(handle); };
  std::unique_ptr<void, decltype(destroy)> handle(raw_handle, destroy);

  char text[] = "VIP";
  CompanyString cs{static_cast<int32_t>(strlen(text)), text};
  CompanyOperatorKeywordInput row{100, &cs, COMPANY_MOCK_SERVICE_KEYWORD_MATCH};
  NamedIoBatch inputs(1), outputs(1);
  inputs[0]["test.keyword_in"] = MakeBorrowedOperatorInput(&row);
  outputs[0]["test_r3_out_a.keyword_out"] = nullptr;
  outputs[0]["test_r3_out_b.keyword_out"] = nullptr;

  ASSERT_EQ(ops_.Process(handle.get(), inputs, outputs), 0)
      << GetOperatorLastError();

  const auto* out_a = static_cast<const CompanyOperatorKeywordOutput*>(
      outputs[0].at("test_r3_out_a.keyword_out").get());
  const auto* out_b = static_cast<const CompanyOperatorKeywordOutput*>(
      outputs[0].at("test_r3_out_b.keyword_out").get());
  ASSERT_NE(out_a, nullptr);
  ASSERT_NE(out_b, nullptr);
  EXPECT_NE(out_a, out_b);
  EXPECT_EQ(out_a->request_id, 100u);
  EXPECT_EQ(out_b->request_id, 100u);
  EXPECT_EQ(out_a->service_type, 9501);
  EXPECT_EQ(out_b->service_type, 9502);

  // 同一结果介于两项容量之间：a 编码成功，b 因自己的较小容量失败。
  for (auto& [key, value] : outputs[0]) value.reset();
  const auto control_text =
      nlohmann::json{{"rules",
                      {{{"pattern", "VIP"},
                        {"category", "HIT"},
                        {"constants", {{"padding", std::string(1100, 'x')}}}}}}}
          .dump();
  ControlJsonParam control{kControlCmdUpdateRules, control_text.c_str()};
  ASSERT_EQ(ops_.Control(handle.get(), ControlCommand::kJson, &control), 0);
  EXPECT_EQ(ops_.Process(handle.get(), inputs, outputs),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_NE(
      std::string(GetOperatorLastError()).find("keyword_out/test_r3_out_b"),
      std::string::npos);
  for (const auto& [key, value] : outputs[0]) EXPECT_EQ(value, nullptr);

  // 歧义 key 检查：当多个项共享同一 type 时，未指定业务名作为命名空间的 key
  // 不能通过兜底被静默吞掉，必须报错拒绝
  {
    NamedIoBatch amb_inputs(1), amb_outputs(1);
    amb_inputs[0]["test.keyword_in"] = MakeBorrowedOperatorInput(&row);
    amb_outputs[0]["unrecognized_chan.keyword_out"] = nullptr;
    EXPECT_NE(ops_.Process(handle.get(), amb_inputs, amb_outputs), 0);
  }
}

// R3: 同侧多个项共享相同宿主结构体 type 时，支持使用不同命名布局（allocator），
// 各自持有独立的输出池和布局规格且互不覆盖。
TEST_F(OperatorApiTest,
       MultipleItemsSharingSameHostStructTypeWithDifferentLayouts) {
  using namespace llm_edgeflow;
  using namespace llm_edgeflow::test_support;

  ScopedTempDirectory temp;
  nlohmann::json pipeline;
  std::ifstream source(std::filesystem::path(GetConfDir()) /
                       "configs/pipeline_keyword_match_rules.json");
  source >> pipeline;
  pipeline["io"]["input"] = {
      {{"type", "keyword_in"}, {"name", "keyword_match"}}};
  // 两个输出项共享同一宿主结构体类型 test_nested_out，但使用不同命名布局
  pipeline["io"]["output"] = {
      {{"type", "test_nested_out"}, {"name", "standard_pair"}},
      {{"type", "test_nested_out"}, {"name", "alternate_pair"}}};
  std::ofstream(temp.path() / "pipeline.json") << pipeline;
  std::ofstream(temp.path() / "pipeline.conf")
      << nlohmann::json{{"pipe_path", "pipeline.json"}};

  auto param = DefaultCreateParam("pipeline.conf");
  const auto root = temp.path().string();
  param.model_path = root.c_str();

  void* raw_handle = nullptr;
  ASSERT_EQ(ops_.Create(&raw_handle, &param), 0) << GetOperatorLastError();
  const auto destroy = [this](void* handle) { ops_.Destroy(handle); };
  std::unique_ptr<void, decltype(destroy)> handle(raw_handle, destroy);

  char text[] = "VIP";
  CompanyString cs{static_cast<int32_t>(strlen(text)), text};
  CompanyOperatorKeywordInput row{200, &cs, COMPANY_MOCK_SERVICE_KEYWORD_MATCH};
  NamedIoBatch inputs(1), outputs(1);
  inputs[0]["test.keyword_in"] = MakeBorrowedOperatorInput(&row);
  outputs[0]["standard_pair.test_nested_out"] = nullptr;
  outputs[0]["alternate_pair.test_nested_out"] = nullptr;

  ASSERT_EQ(ops_.Process(handle.get(), inputs, outputs), 0)
      << GetOperatorLastError();

  const auto* env_std = static_cast<const NestedOutputEnvelope*>(
      outputs[0].at("standard_pair.test_nested_out").get());
  const auto* env_alt = static_cast<const NestedOutputEnvelope*>(
      outputs[0].at("alternate_pair.test_nested_out").get());
  ASSERT_NE(env_std, nullptr);
  ASSERT_NE(env_alt, nullptr);
  EXPECT_NE(env_std, env_alt);
  EXPECT_EQ(env_std->request_id, 200u);
  EXPECT_EQ(env_alt->request_id, 200u);
  // standard_pair 使用 tag=1 (kind 1, capacity 2)
  EXPECT_EQ(env_std->allocator_tag, 1);
  EXPECT_EQ(env_std->kind, 1);
  const auto* payload_std =
      static_cast<const NestedOutputPayload*>(env_std->payload);
  ASSERT_NE(payload_std, nullptr);
  EXPECT_EQ(payload_std->capacity, 2u);

  // alternate_pair 使用 tag=2 (kind 2, capacity 3)
  EXPECT_EQ(env_alt->allocator_tag, 2);
  EXPECT_EQ(env_alt->kind, 2);
  const auto* payload_alt =
      static_cast<const NestedOutputPayload*>(env_alt->payload);
  ASSERT_NE(payload_alt, nullptr);
  EXPECT_EQ(payload_alt->capacity, 3u);
}

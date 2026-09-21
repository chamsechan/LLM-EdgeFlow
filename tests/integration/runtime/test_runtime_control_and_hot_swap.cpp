#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/common_contracts.h"
#include "core/pipeline.h"
#include "edgeflow/operator/interface.h"
#include "edgeflow/operator/types.h"
#include "platform_mock/error_codes.h"
#include "platform_mock/operator_data_types.h"
#include "tests/support/pipeline_test_utils.h"

namespace llm_edgeflow {

class RuntimeControlAndHotSwapTest : public ::testing::Test {
 protected:
  void SetUp() override {
    operator_api::Get_LLM_EDGEFLOW_OperatorTable().Init();
  }
  void TearDown() override {
    operator_api::Get_LLM_EDGEFLOW_OperatorTable().DeInit();
  }
};

namespace {

nlohmann::json ControlInstancesPipeline() {
  return nlohmann::json::parse(R"({
    "biz_name":"keyword_match_v1", "models":[], "pipeline":[
      {"id":"rules_a", "node_type":"TextRuleMatchNode", "depends_on":[],
       "ports":{"inputs":{"text":"input_sentences"},
                "outputs":{"matches":"first_matches"}},
       "config":{"categories":{"INITIAL_A":["sample"]}}},
      {"id":"rules_b", "node_type":"TextRuleMatchNode", "depends_on":[],
       "ports":{"inputs":{"text":"input_sentences"},
                "outputs":{"matches":"rule_matches"}},
       "config":{"categories":{"INITIAL_B":["sample"]}}},
      {"id":"template", "node_type":"TextTemplateNode", "depends_on":[],
       "ports":{"inputs":{"primary":"input_sentences"},
                "outputs":{"text":"rendered"}}}
    ]})");
}

nlohmann::json TargetedRules(const std::string& id,
                             const std::string& category) {
  const nlohmann::json payload = {{"categories", {{category, {"sample"}}}}};
  return {{"$edgeflow_control", 1}, {"node_id", id}, {"payload", payload}};
}

void ExpectRuleCategories(llm_edgeflow::Pipeline* pipeline,
                          const std::string& first, const std::string& second) {
  using namespace llm_edgeflow;
  AlgContext context;
  ASSERT_TRUE(context.Publish("input_sentences", TextBatch{{17, 0, "sample"}}));
  ASSERT_EQ(pipeline->Execute(&context), 0);
  const auto* first_matches = context.Read<RuleMatchBatch>("first_matches");
  const auto* second_matches = context.Read<RuleMatchBatch>("rule_matches");
  ASSERT_NE(first_matches, nullptr);
  ASSERT_NE(second_matches, nullptr);
  ASSERT_EQ(first_matches->size(), 1u);
  ASSERT_EQ(second_matches->size(), 1u);
  EXPECT_EQ(first_matches->front().req_id, 17u);
  EXPECT_EQ(second_matches->front().req_id, 17u);
  EXPECT_EQ(first_matches->front().data.category, first);
  EXPECT_EQ(second_matches->front().data.category, second);
}

}  // namespace

TEST_F(RuntimeControlAndHotSwapTest,
       TargetedControlUpdatesOnlySelectedInstance) {
  using namespace llm_edgeflow;
  Pipeline pipeline;
  PipelineDiagnostic diagnostic;
  ASSERT_TRUE(
      BuildTestPipeline(pipeline, ControlInstancesPipeline(), &diagnostic))
      << diagnostic.message;
  ExpectRuleCategories(&pipeline, "INITIAL_A", "INITIAL_B");
  std::string error;
  ASSERT_EQ(
      pipeline.Control(kControlCmdUpdateRules,
                       TargetedRules("rules_a", "UPDATED_A").dump(), &error),
      0)
      << error;
  ExpectRuleCategories(&pipeline, "UPDATED_A", "INITIAL_B");
  ASSERT_EQ(
      pipeline.Control(kControlCmdUpdateRules,
                       TargetedRules("rules_b", "UPDATED_B").dump(), &error),
      0)
      << error;
  ExpectRuleCategories(&pipeline, "UPDATED_A", "UPDATED_B");

  // Unwrapped payloads retain their existing broadcast behavior.
  ASSERT_EQ(
      pipeline.Control(kControlCmdUpdateRules,
                       R"({"categories":{"BROADCAST":["sample"]}})", &error),
      0)
      << error;
  ExpectRuleCategories(&pipeline, "BROADCAST", "BROADCAST");
}

TEST_F(RuntimeControlAndHotSwapTest,
       InvalidTargetedControlPreservesAllInstances) {
  using namespace llm_edgeflow;
  Pipeline pipeline;
  PipelineDiagnostic diagnostic;
  ASSERT_TRUE(
      BuildTestPipeline(pipeline, ControlInstancesPipeline(), &diagnostic))
      << diagnostic.message;
  const auto valid = TargetedRules("rules_a", "UPDATED");
  std::vector<std::pair<nlohmann::json, std::string>> invalid;
  for (const nlohmann::json& version :
       {nlohmann::json(2), nlohmann::json(1.0), nlohmann::json("1"),
        nlohmann::json(nullptr)}) {
    auto envelope = valid;
    envelope["$edgeflow_control"] = version;
    invalid.emplace_back(std::move(envelope), "$edgeflow_control");
  }
  for (const char* missing : {"node_id", "payload"}) {
    auto envelope = valid;
    envelope.erase(missing);
    invalid.emplace_back(std::move(envelope), missing);
  }
  for (const nlohmann::json& id :
       {nlohmann::json(""), nlohmann::json(42), nlohmann::json("missing")}) {
    auto envelope = valid;
    envelope["node_id"] = id;
    invalid.emplace_back(std::move(envelope), "node_id");
  }
  for (const nlohmann::json& payload :
       {nlohmann::json::array(), nlohmann::json(nullptr),
        nlohmann::json("update")}) {
    auto envelope = valid;
    envelope["payload"] = payload;
    invalid.emplace_back(std::move(envelope), "payload");
  }
  auto envelope = valid;
  envelope["extra"] = true;
  invalid.emplace_back(envelope, "extra");
  envelope = valid;
  envelope["payload"] = {{"categories", {{"UPDATED", 123}}}};
  invalid.emplace_back(envelope, "UPDATED");
  envelope["payload"] = {{"rules", {{{"pattern", "sample"}, {"score", 1.5}}}}};
  invalid.emplace_back(envelope, "maximum");
  envelope["payload"] = {
      {"rules", {{{"pattern", "("}, {"strategy", "regex"}}}}};
  invalid.emplace_back(envelope, "rules_a");
  for (const auto& [payload, field] : invalid) {
    SCOPED_TRACE(payload.dump());
    std::string error;
    EXPECT_NE(pipeline.Control(kControlCmdUpdateRules, payload.dump(), &error),
              0);
    EXPECT_NE(error.find(field), std::string::npos) << error;
    ExpectRuleCategories(&pipeline, "INITIAL_A", "INITIAL_B");
  }
  std::string error;
  EXPECT_EQ(pipeline.Control(kControlCmdUpdateRules,
                             TargetedRules("template", "WRONG_TARGET").dump(),
                             &error),
            -7);
  EXPECT_NE(error.find("template"), std::string::npos) << error;
  ExpectRuleCategories(&pipeline, "INITIAL_A", "INITIAL_B");
}

// 1. 关键词库运行时动态热更新与立即生效测试
TEST_F(RuntimeControlAndHotSwapTest, KeywordMatcherDynamicHotSwap) {
  auto op = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  operator_api::CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = operator_api::ComputePlatform::kCpu;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(op.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  // 1.1 初始状态测试：默认无 VIP_URGENT 命中
  std::string input_text_1 = "这是一个普通的测试，包含 VIP 专席客户服务。";
  CompanyString cs1{static_cast<int32_t>(input_text_1.size()),
                    const_cast<char*>(input_text_1.data())};
  CompanyOperatorKeywordInput in_req_1{10001, &cs1};

  operator_api::NamedIoBatch inputs_1(1);
  inputs_1[0]["client_channel.keyword_in"] =
      operator_api::MakeBorrowedOperatorInput(&in_req_1);
  operator_api::NamedIoBatch outputs_1(1);
  outputs_1[0]["client_channel.keyword_out"] = nullptr;

  int ret = op.Process(handle, inputs_1, outputs_1);
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(outputs_1.size(), 1u);
  auto out1_sp = outputs_1[0]["client_channel.keyword_out"];
  ASSERT_NE(out1_sp, nullptr);
  auto* out_res_1 = static_cast<CompanyOperatorKeywordOutput*>(out1_sp.get());
  EXPECT_EQ(out_res_1->is_hit, 0);

  // 1.2 运行时热下发新词库类别 "VIP_URGENT": ["VIP", "专席"]
  nlohmann::json control_param = {{"categories",
                                   {{"VIP_URGENT", {"VIP", "专席"}},
                                    {"DISCOUNT_PROMO", {"返现", "优惠券"}}}}};
  const nlohmann::json envelope = {{"$edgeflow_control", 1},
                                   {"node_id", "node_0_TextRuleMatchNode"},
                                   {"payload", control_param}};
  std::string param_str = envelope.dump();

  operator_api::ControlUpdateRulesParam ctrl{param_str.c_str()};
  ret = op.Control(handle, operator_api::ControlCommand::kUpdateRules, &ctrl);
  EXPECT_EQ(ret, 0);

  // 1.3 再次执行匹配，验证新词库已即时生效并命中
  outputs_1[0]["client_channel.keyword_out"] = nullptr;
  ret = op.Process(handle, inputs_1, outputs_1);
  EXPECT_EQ(ret, 0);
  out1_sp = outputs_1[0]["client_channel.keyword_out"];
  ASSERT_NE(out1_sp, nullptr);
  out_res_1 = static_cast<CompanyOperatorKeywordOutput*>(out1_sp.get());
  EXPECT_EQ(out_res_1->is_hit, 1);
  ASSERT_NE(out_res_1->match_result_json, nullptr);
  std::string match_str(out_res_1->match_result_json->data,
                        out_res_1->match_result_json->length);
  nlohmann::json match_json = nlohmann::json::parse(match_str);
  ASSERT_TRUE(match_json.contains("matches"));
  ASSERT_FALSE(match_json["matches"].empty());
  EXPECT_EQ(match_json["matches"][0]["category"], "VIP_URGENT");

  // 1.4 验证第二条新词库 DISCOUNT_PROMO
  std::string input_text_2 = "扫码立即返现50元优惠券！";
  CompanyString cs2{static_cast<int32_t>(input_text_2.size()),
                    const_cast<char*>(input_text_2.data())};
  CompanyOperatorKeywordInput in_req_2{10002, &cs2};

  operator_api::NamedIoBatch inputs_2(1);
  inputs_2[0]["client_channel.keyword_in"] =
      operator_api::MakeBorrowedOperatorInput(&in_req_2);
  operator_api::NamedIoBatch outputs_2(1);
  outputs_2[0]["client_channel.keyword_out"] = nullptr;

  ret = op.Process(handle, inputs_2, outputs_2);
  EXPECT_EQ(ret, 0);
  auto out2_sp = outputs_2[0]["client_channel.keyword_out"];
  ASSERT_NE(out2_sp, nullptr);
  auto* out_res_2 = static_cast<CompanyOperatorKeywordOutput*>(out2_sp.get());
  EXPECT_EQ(out_res_2->is_hit, 1);
  ASSERT_NE(out_res_2->match_result_json, nullptr);
  std::string match_str_2(out_res_2->match_result_json->data,
                          out_res_2->match_result_json->length);
  nlohmann::json match_json_2 = nlohmann::json::parse(match_str_2);
  ASSERT_TRUE(match_json_2.contains("matches"));
  ASSERT_FALSE(match_json_2["matches"].empty());
  EXPECT_EQ(match_json_2["matches"][0]["category"], "DISCOUNT_PROMO");

  out1_sp.reset();
  out2_sp.reset();
  outputs_1.clear();
  outputs_2.clear();
  op.Destroy(handle);
}

// 2. 同一 handle 的 Process/Control 由 Operator 层串行化，停流 join 后再销毁
TEST_F(RuntimeControlAndHotSwapTest, ConcurrentProcessAndHotControl) {
  auto op = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  operator_api::CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = operator_api::ComputePlatform::kCpu;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(op.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::atomic<bool> start_flag{false};
  std::atomic<int> process_count{0};
  std::atomic<int> control_count{0};
  constexpr int kProcessIterations = 64;
  constexpr int kControlIterations = 16;

  // 线程 1: 持续发起推理
  std::thread process_thread([&]() {
    while (!start_flag.load()) std::this_thread::yield();
    std::string text = "测试动态控制下的并发推理稳定性，含有VIP关键词";
    CompanyString cs{static_cast<int32_t>(text.size()),
                     const_cast<char*>(text.data())};
    for (int iteration = 0; iteration < kProcessIterations; ++iteration) {
      CompanyOperatorKeywordInput in_req{10003, &cs};
      operator_api::NamedIoBatch inputs(1);
      inputs[0]["client_channel.keyword_in"] =
          operator_api::MakeBorrowedOperatorInput(&in_req);
      operator_api::NamedIoBatch outputs(1);
      outputs[0]["client_channel.keyword_out"] = nullptr;
      int ret = op.Process(handle, inputs, outputs);
      if (ret == 0) {
        process_count.fetch_add(1);
      }
    }
  });

  // 线程 2: 持续发起词表热更新
  std::thread control_thread([&]() {
    while (!start_flag.load()) std::this_thread::yield();
    for (int iter = 0; iter < kControlIterations; ++iter) {
      nlohmann::json ctrl_json = {{"categories",
                                   {{"DYNAMIC_CAT_" + std::to_string(iter % 5),
                                     {"VIP", "测试", "动态"}}}}};
      std::string s = ctrl_json.dump();
      operator_api::ControlUpdateRulesParam ctrl{s.c_str()};
      int ret =
          op.Control(handle, operator_api::ControlCommand::kUpdateRules, &ctrl);
      if (ret == 0) {
        control_count.fetch_add(1);
      }
    }
  });

  start_flag.store(true);

  process_thread.join();
  control_thread.join();

  EXPECT_EQ(process_count.load(), kProcessIterations);
  EXPECT_EQ(control_count.load(), kControlIterations);

  EXPECT_EQ(op.Destroy(handle), 0);
}

// 3. 非法控制指令与边界容错测试
TEST_F(RuntimeControlAndHotSwapTest, InvalidControlCommands) {
  auto op = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  operator_api::CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = operator_api::ComputePlatform::kCpu;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(op.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  // 3.1 非法指令码 cmd = 99999 (未声明命令返回
  // COMPANY_ALG_ERR_UNSUPPORTED_CONTROL = -7)
  operator_api::ControlJsonParam ctrl1{99999, "{}"};
  int ret = op.Control(handle, operator_api::ControlCommand::kJson, &ctrl1);
  EXPECT_EQ(ret, COMPANY_ALG_ERR_UNSUPPORTED_CONTROL);

  // 3.2 空指针参数
  ret = op.Control(handle, operator_api::ControlCommand::kUpdateRules, nullptr);
  EXPECT_EQ(ret, -2);  // adapter 层拦截空结构体指针

  operator_api::ControlUpdateRulesParam ctrl_null_str{nullptr};
  ret = op.Control(handle, operator_api::ControlCommand::kUpdateRules,
                   &ctrl_null_str);
  EXPECT_EQ(ret, -2);  // adapter 层拦截空 JSON 字符串指针

  // 3.3 畸形 JSON 字符串
  operator_api::ControlUpdateRulesParam ctrl2{"{invalid_json_missing_brace"};
  ret = op.Control(handle, operator_api::ControlCommand::kUpdateRules, &ctrl2);
  EXPECT_EQ(ret, -2);  // adapter 层拦截畸形 JSON 并返回 -2

  // 3.4 空句柄控制
  ret = op.Control(nullptr, operator_api::ControlCommand::kJson, &ctrl1);
  EXPECT_EQ(ret, -1);

  op.Destroy(handle);
}

}  // namespace llm_edgeflow

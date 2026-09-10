#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/common_contracts.h"
#include "core/pipeline.h"
#include "edgeflow/c_api.h"
#include "edgeflow/c_api.hpp"

static std::string GetConfigPath(const std::string& rel_path) {
  FILE* fp = fopen(rel_path.c_str(), "r");
  if (fp) {
    fclose(fp);
    return rel_path;
  }
  return "../" + rel_path;
}

class RuntimeControlAndHotSwapTest : public ::testing::Test {
 protected:
  void SetUp() override { Alg_Init(); }
  void TearDown() override { Alg_DeInit(); }
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
  ASSERT_TRUE(pipeline.BuildFromJson(ControlInstancesPipeline(), &diagnostic))
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
  ASSERT_TRUE(pipeline.BuildFromJson(ControlInstancesPipeline(), &diagnostic))
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
  std::string cfg_path =
      GetConfigPath("configs/pipeline_keyword_match_rules.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg_path.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

  void* handle = nullptr;
  ASSERT_EQ(Alg_Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  // 1.1 初始状态测试：默认无 VIP_URGENT 命中
  const char* input_text_1 = "这是一个普通的测试，包含 VIP 专席客户服务。";
  CompanyKeywordInputStruct in_req_1{10001, input_text_1};
  std::vector<void*> inputs_1 = {&in_req_1};
  CompanyKeywordOutputStruct out_res_1;
  std::vector<void*> outputs_1 = {&out_res_1};

  int ret = Alg_Process(handle, inputs_1, outputs_1);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out_res_1.is_hit, 0);

  // 1.2 运行时热下发新词库类别 "VIP_URGENT": ["VIP", "专席"]
  nlohmann::json control_param = {{"categories",
                                   {{"VIP_URGENT", {"VIP", "专席"}},
                                    {"DISCOUNT_PROMO", {"返现", "优惠券"}}}}};
  const nlohmann::json envelope = {{"$edgeflow_control", 1},
                                   {"node_id", "node_0_TextRuleMatchNode"},
                                   {"payload", control_param}};
  std::string param_str = envelope.dump();

  CompanyAlgParamControl ctrl;
  ctrl.control_cmd = 1;
  ctrl.json_param_str = param_str.c_str();
  ret = Alg_Control(handle, &ctrl);
  EXPECT_EQ(ret, 0);

  // 1.3 再次执行匹配，验证新词库已即时生效并命中
  ret = Alg_Process(handle, inputs_1, outputs_1);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out_res_1.is_hit, 1);
  nlohmann::json match_json =
      nlohmann::json::parse(out_res_1.match_result_json);
  ASSERT_TRUE(match_json.contains("matches"));
  ASSERT_FALSE(match_json["matches"].empty());
  EXPECT_EQ(match_json["matches"][0]["category"], "VIP_URGENT");

  // 1.4 验证第二条新词库 DISCOUNT_PROMO
  const char* input_text_2 = "扫码立即返现50元优惠券！";
  CompanyKeywordInputStruct in_req_2{10002, input_text_2};
  std::vector<void*> inputs_2 = {&in_req_2};
  CompanyKeywordOutputStruct out_res_2;
  std::vector<void*> outputs_2 = {&out_res_2};

  ret = Alg_Process(handle, inputs_2, outputs_2);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out_res_2.is_hit, 1);
  nlohmann::json match_json_2 =
      nlohmann::json::parse(out_res_2.match_result_json);
  ASSERT_TRUE(match_json_2.contains("matches"));
  ASSERT_FALSE(match_json_2["matches"].empty());
  EXPECT_EQ(match_json_2["matches"][0]["category"], "DISCOUNT_PROMO");

  Alg_Destroy(handle);
}

// 2. 同一 handle 的 Process/Control 由 C ABI 层串行化，停流 join 后再销毁
TEST_F(RuntimeControlAndHotSwapTest, ConcurrentProcessAndHotControl) {
  std::string cfg_path =
      GetConfigPath("configs/pipeline_keyword_match_rules.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg_path.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

  void* handle = nullptr;
  ASSERT_EQ(Alg_Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::atomic<bool> start_flag{false};
  std::atomic<int> process_count{0};
  std::atomic<int> control_count{0};
  constexpr int kProcessIterations = 64;
  constexpr int kControlIterations = 16;

  // 线程 1: 持续发起推理
  std::thread process_thread([&]() {
    while (!start_flag.load()) std::this_thread::yield();
    const char* text = "测试动态控制下的并发推理稳定性，含有VIP关键词";
    for (int iteration = 0; iteration < kProcessIterations; ++iteration) {
      CompanyKeywordInputStruct in_req{10003, text};
      std::vector<void*> inputs = {&in_req};
      CompanyKeywordOutputStruct out_res;
      std::vector<void*> outputs = {&out_res};
      int ret = Alg_Process(handle, inputs, outputs);
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
      CompanyAlgParamControl ctrl;
      ctrl.control_cmd = 1;
      ctrl.json_param_str = s.c_str();
      int ret = Alg_Control(handle, &ctrl);
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

  EXPECT_EQ(Alg_Destroy(handle), 0);
}

// 3. 非法控制指令与边界容错测试
TEST_F(RuntimeControlAndHotSwapTest, InvalidControlCommands) {
  std::string cfg_path =
      GetConfigPath("configs/pipeline_keyword_match_rules.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg_path.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

  void* handle = nullptr;
  ASSERT_EQ(Alg_Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  // 3.1 非法指令码 cmd = 99999 (未声明命令返回
  // COMPANY_ALG_ERR_UNSUPPORTED_CONTROL = -7)
  CompanyAlgParamControl ctrl1{99999, "{}"};
  int ret = Alg_Control(handle, &ctrl1);
  EXPECT_EQ(ret, COMPANY_ALG_ERR_UNSUPPORTED_CONTROL);

  // 3.2 空指针参数
  ret = Alg_Control(handle, nullptr);
  EXPECT_EQ(ret, -1);  // adapter 层拦截空结构体指针

  CompanyAlgParamControl ctrl_null_str{1, nullptr};
  ret = Alg_Control(handle, &ctrl_null_str);
  EXPECT_EQ(ret, -2);  // adapter 层拦截空 JSON 字符串指针

  // 3.3 畸形 JSON 字符串
  CompanyAlgParamControl ctrl2{1, "{invalid_json_missing_brace"};
  ret = Alg_Control(handle, &ctrl2);
  EXPECT_EQ(ret, -1);  // node 层捕获 parse 异常并返回 -1

  // 3.4 空句柄控制
  ret = Alg_Control(nullptr, &ctrl1);
  EXPECT_EQ(ret, -1);

  Alg_Destroy(handle);
}

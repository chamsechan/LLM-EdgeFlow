#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "company_alg_cpp.hpp"
#include "company_alg_interface.h"

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

// 1. 关键词库运行时动态热更新与立即生效测试
TEST_F(RuntimeControlAndHotSwapTest, KeywordMatcherDynamicHotSwap) {
  std::string cfg_path = GetConfigPath("configs/pipeline_keyword_match.json");
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
  CompanyString in_str_1;
  CompanyString_FromCString(&in_str_1, input_text_1);
  CompanyKeywordInputStruct in_req_1{10001, &in_str_1};
  std::vector<const void*> inputs_1 = {&in_req_1};

  char out_buf_1[2048] = {0};
  CompanyString out_str_1;
  CompanyString_Init(&out_str_1, out_buf_1, sizeof(out_buf_1));
  CompanyKeywordOutputStruct out_res_1{.match_result_json = &out_str_1};
  std::vector<void*> outputs_1 = {&out_res_1};

  int num_outputs_1 = 1;
  int ret =
      Alg_Process(handle, inputs_1.data(), 1, outputs_1.data(), &num_outputs_1);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out_res_1.is_hit, 0);

  // 1.2 运行时热下发新词库类别 "VIP_URGENT": ["VIP", "专席"]
  nlohmann::json control_param = {{"categories",
                                   {{"VIP_URGENT", {"VIP", "专席"}},
                                    {"DISCOUNT_PROMO", {"返现", "优惠券"}}}}};
  std::string param_str = control_param.dump();

  CompanyAlgParamControl ctrl;
  ctrl.control_cmd = 1;
  ctrl.json_param_str = param_str.c_str();
  ret = Alg_Control(handle, &ctrl);
  EXPECT_EQ(ret, 0);

  // 1.3 再次执行匹配，验证新词库已即时生效并命中
  num_outputs_1 = 1;
  ret =
      Alg_Process(handle, inputs_1.data(), 1, outputs_1.data(), &num_outputs_1);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out_res_1.is_hit, 1);
  nlohmann::json match_json =
      nlohmann::json::parse(out_res_1.match_result_json->data);
  ASSERT_TRUE(match_json.contains("matches"));
  ASSERT_FALSE(match_json["matches"].empty());
  EXPECT_EQ(match_json["matches"][0]["category"], "VIP_URGENT");

  // 1.4 验证第二条新词库 DISCOUNT_PROMO
  const char* input_text_2 = "扫码立即返现50元优惠券！";
  CompanyString in_str_2;
  CompanyString_FromCString(&in_str_2, input_text_2);
  CompanyKeywordInputStruct in_req_2{10002, &in_str_2};
  std::vector<const void*> inputs_2 = {&in_req_2};

  char out_buf_2[2048] = {0};
  CompanyString out_str_2;
  CompanyString_Init(&out_str_2, out_buf_2, sizeof(out_buf_2));
  CompanyKeywordOutputStruct out_res_2{.match_result_json = &out_str_2};
  std::vector<void*> outputs_2 = {&out_res_2};

  int num_outputs_2 = 1;
  ret =
      Alg_Process(handle, inputs_2.data(), 1, outputs_2.data(), &num_outputs_2);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out_res_2.is_hit, 1);
  nlohmann::json match_json_2 =
      nlohmann::json::parse(out_res_2.match_result_json->data);
  ASSERT_TRUE(match_json_2.contains("matches"));
  ASSERT_FALSE(match_json_2["matches"].empty());
  EXPECT_EQ(match_json_2["matches"][0]["category"], "DISCOUNT_PROMO");

  Alg_Destroy(handle);
}

// 2. 在线并发执行与热控制竞争测试 (Process and Control Concurrency)
TEST_F(RuntimeControlAndHotSwapTest, ConcurrentProcessAndHotControl) {
  std::string cfg_path = GetConfigPath("configs/pipeline_keyword_match.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg_path.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

  void* handle = nullptr;
  ASSERT_EQ(Alg_Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::atomic<bool> stop_flag{false};
  std::atomic<int> process_count{0};
  std::atomic<int> control_count{0};

  // 线程 1: 持续发起推理
  std::thread process_thread([&]() {
    const char* text = "测试动态控制下的并发推理稳定性，含有VIP关键词";
    while (!stop_flag.load()) {
      CompanyString in_str;
      CompanyString_FromCString(&in_str, text);
      CompanyKeywordInputStruct in_req{10003, &in_str};
      std::vector<const void*> inputs = {&in_req};

      char out_buf[2048] = {0};
      CompanyString out_str;
      CompanyString_Init(&out_str, out_buf, sizeof(out_buf));
      CompanyKeywordOutputStruct out_res{.match_result_json = &out_str};
      std::vector<void*> outputs = {&out_res};

      int num_outs = 1;
      int ret =
          Alg_Process(handle, inputs.data(), 1, outputs.data(), &num_outs);
      if (ret == 0) {
        process_count.fetch_add(1);
      }
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  });

  // 线程 2: 持续发起词表热更新
  std::thread control_thread([&]() {
    int iter = 0;
    while (!stop_flag.load()) {
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
      iter++;
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
  });

  // 运行 200 毫秒高频并发交互
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  stop_flag.store(true);

  process_thread.join();
  control_thread.join();

  EXPECT_GT(process_count.load(), 50);
  EXPECT_GT(control_count.load(), 10);

  Alg_Destroy(handle);
}

// 3. 非法控制指令与边界容错测试
TEST_F(RuntimeControlAndHotSwapTest, InvalidControlCommands) {
  std::string cfg_path = GetConfigPath("configs/pipeline_keyword_match.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg_path.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

  void* handle = nullptr;
  ASSERT_EQ(Alg_Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  // 3.1 非法指令码 cmd = 99999 (未识别命令默认忽略返回 0)
  CompanyAlgParamControl ctrl1{99999, "{}"};
  int ret = Alg_Control(handle, &ctrl1);
  EXPECT_EQ(ret, 0);

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

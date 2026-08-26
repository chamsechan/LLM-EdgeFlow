#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "company_alg_cpp.hpp"
#include "company_alg_interface.h"

std::string GetConfigPath(const std::string& rel_path) {
  FILE* fp = fopen(rel_path.c_str(), "r");
  if (fp) {
    fclose(fp);
    return rel_path;
  }
  return "../" + rel_path;
}

namespace alg_framework {

class ConcurrencyAndEdgeCasesTest : public ::testing::Test {
 protected:
  void SetUp() override { Alg_Init(); }
  void TearDown() override { Alg_DeInit(); }
};

// 1. 多线程高并发句柄独立运行与竞争测试 (8 个 Worker 线程并发 20 轮全生命周期)
TEST_F(ConcurrencyAndEdgeCasesTest, MultiThreadedConcurrentStressTest) {
  const int num_threads = 8;
  const int iterations_per_thread = 20;
  std::atomic<int> success_count{0};
  std::atomic<int> error_count{0};

  std::vector<std::thread> workers;
  workers.reserve(num_threads);

  std::string cfg_path = GetConfigPath("configs/pipeline_keyword_match.json");

  for (int t = 0; t < num_threads; ++t) {
    workers.emplace_back([&, t]() {
      CompanyAlgParamCreate param;
      param.config_file_path = cfg_path.c_str();
      param.model_root_dir = "./models";
      param.device_id = 0;
      param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

      for (int iter = 0; iter < iterations_per_thread; ++iter) {
        void* handle = nullptr;
        int ret = Alg_Create(&handle, &param);
        if (ret != 0 || !handle) {
          error_count++;
          continue;
        }

        // 动态下发规则
        CompanyAlgParamControl ctrl;
        ctrl.control_cmd = 1;
        std::string rule_json = "{\"categories\": {\"THREAD_VIP_" +
                                std::to_string(t) + "\": [\"VIP" +
                                std::to_string(t) + "\"]}}";
        ctrl.json_param_str = rule_json.c_str();
        Alg_Control(handle, &ctrl);

        // 执行推理
        std::string query = "客户请求VIP" + std::to_string(t) + "专席服务";
        CompanyString in_str;
        CompanyString_FromCString(&in_str, query.c_str());
        CompanyKeywordInputStruct in_req{static_cast<uint64_t>(t * 1000 + iter),
                                         &in_str};
        std::vector<const void*> inputs = {&in_req};

        char out_buf[2048] = {0};
        CompanyString out_str;
        CompanyString_Init(&out_str, out_buf, sizeof(out_buf));
        CompanyKeywordOutputStruct out_res{.match_result_json = &out_str};
        std::vector<void*> outputs = {&out_res};

        int num_outputs = 1;
        ret =
            Alg_Process(handle, inputs.data(), 1, outputs.data(), &num_outputs);
        if (ret == 0 && out_res.is_hit == 1) {
          success_count++;
        } else {
          error_count++;
        }

        ret = Alg_Destroy(handle);
        if (ret != 0) {
          error_count++;
        }
      }
    });
  }

  for (auto& w : workers) {
    w.join();
  }

  EXPECT_EQ(error_count.load(), 0);
  EXPECT_EQ(success_count.load(), num_threads * iterations_per_thread);
}

// 2. 极端边界与畸形数据鲁棒性测试 (Edge Cases & Fault Tolerance)
TEST_F(ConcurrencyAndEdgeCasesTest, EdgeCasesAndFaultTolerance) {
  // Case A: 畸形与非法 JSON 传入 Alg_Control
  {
    std::string cfg_path = GetConfigPath("configs/pipeline_keyword_match.json");
    CompanyAlgParamCreate param;
    param.config_file_path = cfg_path.c_str();
    param.model_root_dir = "./models";
    param.device_id = 0;
    param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

    void* handle = nullptr;
    int ret = Alg_Create(&handle, &param);
    ASSERT_EQ(ret, 0);

    CompanyAlgParamControl ctrl_invalid;
    ctrl_invalid.control_cmd = 1;
    ctrl_invalid.json_param_str = "{invalid_malformed_json...";  // 畸形 JSON
    ret = Alg_Control(handle, &ctrl_invalid);
    // 框架应安全拦截并返回错误码，决不能崩溃
    EXPECT_NE(ret, 0);

    // 传入空字符串
    ctrl_invalid.json_param_str = "";
    ret = Alg_Control(handle, &ctrl_invalid);
    EXPECT_NE(ret, 0);

    // 传入空指针
    ctrl_invalid.json_param_str = nullptr;
    ret = Alg_Control(handle, &ctrl_invalid);
    EXPECT_NE(ret, 0);

    Alg_Destroy(handle);
  }

  // Case B: 空文本与纯标点符号输入
  {
    std::string cfg_path = GetConfigPath("configs/pipeline_keyword_match.json");
    CompanyAlgParamCreate param;
    param.config_file_path = cfg_path.c_str();
    param.model_root_dir = "./models";
    param.device_id = 0;
    param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

    void* handle = nullptr;
    Alg_Create(&handle, &param);

    CompanyString empty_str, symbols_str;
    CompanyString_FromCString(&empty_str, "");
    CompanyString_FromCString(&symbols_str, "  !@#$%^&*()_+~`|}{[]:;?><,./  ");

    CompanyKeywordInputStruct empty_req{99901, &empty_str};  // 空文本
    CompanyKeywordInputStruct symbols_req{99902, &symbols_str};
    std::vector<const void*> inputs = {&empty_req, &symbols_req};

    char buf0[2048] = {0}, buf1[2048] = {0};
    CompanyString out_str0, out_str1;
    CompanyString_Init(&out_str0, buf0, sizeof(buf0));
    CompanyString_Init(&out_str1, buf1, sizeof(buf1));

    CompanyKeywordOutputStruct out0{.match_result_json = &out_str0};
    CompanyKeywordOutputStruct out1{.match_result_json = &out_str1};
    std::vector<void*> outputs = {&out0, &out1};

    int num_outputs = 2;
    int ret =
        Alg_Process(handle, inputs.data(), 2, outputs.data(), &num_outputs);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(out0.is_hit, 0);
    EXPECT_EQ(out1.is_hit, 0);

    Alg_Destroy(handle);
  }

  // Case C: 音频 0 采样点边界
  {
    std::string cfg_path =
        GetConfigPath("configs/pipeline_audio_asr_intent.json");
    CompanyAlgParamCreate param;
    param.config_file_path = cfg_path.c_str();
    param.model_root_dir = "./models";
    param.device_id = 0;
    param.biz_type = ALG_BIZ_TYPE_AUDIO_ASR_INTENT;

    void* handle = nullptr;
    int ret = Alg_Create(&handle, &param);
    ASSERT_EQ(ret, 0);

    CompanyAudioInputStruct empty_audio{99903, nullptr, 0, 16000};
    std::vector<const void*> inputs = {&empty_audio};

    char asr_buf[512] = {0}, slot_buf[1024] = {0};
    CompanyString asr_str, slot_str;
    CompanyString_Init(&asr_str, asr_buf, sizeof(asr_buf));
    CompanyString_Init(&slot_str, slot_buf, sizeof(slot_buf));
    CompanyAudioOutputStruct out_audio{.transcribed_text = &asr_str,
                                       .intent_slot_json = &slot_str};
    std::vector<void*> outputs = {&out_audio};

    int num_outputs = 1;
    ret = Alg_Process(handle, inputs.data(), 1, outputs.data(), &num_outputs);
    EXPECT_EQ(ret, 0);

    Alg_Destroy(handle);
  }
}

}  // namespace alg_framework

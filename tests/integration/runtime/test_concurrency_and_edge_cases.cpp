#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "edgeflow/operator/interface.h"
#include "edgeflow/operator/types.h"
#include "platform_mock/operator_data_types.h"

namespace llm_edgeflow {

class ConcurrencyAndEdgeCasesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    operator_api::Get_LLM_EDGEFLOW_OperatorTable().Init();
  }
  void TearDown() override {
    operator_api::Get_LLM_EDGEFLOW_OperatorTable().Deinit();
  }
};

// 1. 多线程高并发句柄独立运行与竞争测试 (8 个 Worker 线程并发 20 轮全生命周期)
TEST_F(ConcurrencyAndEdgeCasesTest, MultiThreadedConcurrentStressTest) {
  const int num_threads = 8;
  const int iterations_per_thread = 20;
  std::atomic<int> success_count{0};
  std::atomic<int> error_count{0};

  std::vector<std::thread> workers;
  workers.reserve(num_threads);

  auto op = operator_api::Get_LLM_EDGEFLOW_OperatorTable();

  for (int t = 0; t < num_threads; ++t) {
    workers.emplace_back([&, t]() {
      operator_api::CreateParam param{};
      param.model_path = ".";
      param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
      param.device_id = 0;
      param.compute_platform = operator_api::ComputePlatform::kCpu;
      param.max_frame_depth = 25;

      for (int iter = 0; iter < iterations_per_thread; ++iter) {
        void* handle = nullptr;
        int ret = op.Create(&handle, &param);
        if (ret != 0 || !handle) {
          error_count++;
          continue;
        }

        // 动态下发规则
        std::string rule_json = "{\"categories\": {\"THREAD_VIP_" +
                                std::to_string(t) + "\": [\"VIP" +
                                std::to_string(t) + "\"]}}";
        operator_api::ControlUpdateRulesParam ctrl{rule_json.c_str()};
        op.Control(handle, operator_api::ControlCommand::kUpdateRules, &ctrl);

        // 执行推理
        std::string query = "客户请求VIP" + std::to_string(t) + "专席服务";
        CompanyString cs{static_cast<int32_t>(query.size()),
                         const_cast<char*>(query.data())};
        CompanyOperatorKeywordInput in_req{
            static_cast<uint64_t>(t * 1000 + iter), &cs};

        operator_api::NamedIoBatch inputs(1);
        inputs[0]["client_channel.keyword_in"] =
            operator_api::MakeBorrowedOperatorInput(&in_req);
        operator_api::NamedIoBatch outputs(1);
        outputs[0]["client_channel.keyword_out"] = nullptr;

        ret = op.Process(handle, inputs, outputs);
        if (ret == 0 && !outputs.empty()) {
          auto out_sp = outputs[0]["client_channel.keyword_out"];
          if (out_sp) {
            auto* out_res =
                static_cast<CompanyOperatorKeywordOutput*>(out_sp.get());
            if (out_res && out_res->is_hit == 1) {
              success_count++;
            } else {
              error_count++;
            }
          } else {
            error_count++;
          }
        } else {
          error_count++;
        }

        outputs.clear();
        ret = op.Destroy(handle);
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
  auto op = operator_api::Get_LLM_EDGEFLOW_OperatorTable();

  // Case A: 畸形与非法 JSON 传入 Control
  {
    operator_api::CreateParam param{};
    param.model_path = ".";
    param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
    param.device_id = 0;
    param.compute_platform = operator_api::ComputePlatform::kCpu;
    param.max_frame_depth = 25;

    void* handle = nullptr;
    int ret = op.Create(&handle, &param);
    ASSERT_EQ(ret, 0);

    operator_api::ControlUpdateRulesParam ctrl_invalid;
    ctrl_invalid.rules_json_str = "{invalid_malformed_json...";  // 畸形 JSON
    ret = op.Control(handle, operator_api::ControlCommand::kUpdateRules,
                     &ctrl_invalid);
    // 框架应安全拦截并返回错误码，决不能崩溃
    EXPECT_NE(ret, 0);

    // 传入空字符串
    ctrl_invalid.rules_json_str = "";
    ret = op.Control(handle, operator_api::ControlCommand::kUpdateRules,
                     &ctrl_invalid);
    EXPECT_NE(ret, 0);

    // 传入空指针
    ctrl_invalid.rules_json_str = nullptr;
    ret = op.Control(handle, operator_api::ControlCommand::kUpdateRules,
                     &ctrl_invalid);
    EXPECT_NE(ret, 0);

    op.Destroy(handle);
  }

  // Case B: 空文本与纯标点符号输入
  {
    operator_api::CreateParam param{};
    param.model_path = ".";
    param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
    param.device_id = 0;
    param.compute_platform = operator_api::ComputePlatform::kCpu;
    param.max_frame_depth = 25;

    void* handle = nullptr;
    int ret = op.Create(&handle, &param);
    ASSERT_EQ(ret, 0);

    std::string empty_str = "";
    std::string symbols_str = "  !@#$%^&*()_+~`|}{[]:;?><,./  ";
    CompanyString cs_empty{static_cast<int32_t>(empty_str.size()),
                           const_cast<char*>(empty_str.data())};
    CompanyString cs_symbols{static_cast<int32_t>(symbols_str.size()),
                             const_cast<char*>(symbols_str.data())};

    CompanyOperatorKeywordInput empty_req{99901, &cs_empty};
    CompanyOperatorKeywordInput symbols_req{99902, &cs_symbols};

    operator_api::NamedIoBatch inputs(2);
    inputs[0]["client_channel.keyword_in"] =
        operator_api::MakeBorrowedOperatorInput(&empty_req);
    inputs[1]["client_channel.keyword_in"] =
        operator_api::MakeBorrowedOperatorInput(&symbols_req);

    operator_api::NamedIoBatch outputs(2);
    outputs[0]["client_channel.keyword_out"] = nullptr;
    outputs[1]["client_channel.keyword_out"] = nullptr;

    ret = op.Process(handle, inputs, outputs);
    EXPECT_EQ(ret, 0);
    ASSERT_EQ(outputs.size(), 2u);

    auto out0_sp = outputs[0]["client_channel.keyword_out"];
    auto out1_sp = outputs[1]["client_channel.keyword_out"];
    ASSERT_NE(out0_sp, nullptr);
    ASSERT_NE(out1_sp, nullptr);

    auto* out0 = static_cast<CompanyOperatorKeywordOutput*>(out0_sp.get());
    auto* out1 = static_cast<CompanyOperatorKeywordOutput*>(out1_sp.get());

    EXPECT_EQ(out0->is_hit, 0);
    EXPECT_EQ(out1->is_hit, 0);

    out0_sp.reset();
    out1_sp.reset();
    outputs.clear();
    op.Destroy(handle);
  }

  // Case C: 音频 0 采样点边界
  {
    operator_api::CreateParam param{};
    param.model_path = ".";
    param.cfg_file_name = "demo/fixtures/mock/pipeline_audio_asr_intent.conf";
    param.device_id = 0;
    param.compute_platform = operator_api::ComputePlatform::kCpu;
    param.max_frame_depth = 25;

    void* handle = nullptr;
    int ret = op.Create(&handle, &param);
    ASSERT_EQ(ret, 0);

    CompanyOperatorAudioInput empty_audio{99903, nullptr, 0, 16000};
    operator_api::NamedIoBatch inputs(1);
    inputs[0]["mic_0.audio_in"] =
        operator_api::MakeBorrowedOperatorInput(&empty_audio);

    operator_api::NamedIoBatch outputs(1);
    outputs[0]["mic_0.audio_out"] = nullptr;

    ret = op.Process(handle, inputs, outputs);
    EXPECT_EQ(ret, 0);

    outputs.clear();
    op.Destroy(handle);
  }
}

}  // namespace llm_edgeflow

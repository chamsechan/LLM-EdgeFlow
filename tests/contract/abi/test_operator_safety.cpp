#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "adapter/io_converter_registry.h"
#include "adapter/operator/mock/platform_value_binding.h"
#include "core/common_contracts.h"
#include "edgeflow/operator/interface.h"
#include "edgeflow/operator/types.h"
#include "platform_mock/error_codes.h"
#include "tests/support/adapter_harness.h"
#include "tests/support/adapter_test_views.h"

namespace llm_edgeflow {
namespace {
constexpr auto kEntities =
    MakeBlackboardKey<StructuredDocumentBatch>("entities");
}  // namespace
}  // namespace llm_edgeflow

using namespace llm_edgeflow::operator_api;

class OperatorSafetyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    llm_edgeflow::IoConverterRegistry::Instance().ResetConflictForTesting();
    Get_LLM_EDGEFLOW_OperatorTable().Init();
  }
  void TearDown() override {
    Get_LLM_EDGEFLOW_OperatorTable().DeInit();
    llm_edgeflow::IoConverterRegistry::Instance().ResetConflictForTesting();
  }
};

// 1. 测试空指针与异常安全防御机制 (noexcept barrier)
TEST_F(OperatorSafetyTest, NullPointerSafety) {
  auto op = Get_LLM_EDGEFLOW_OperatorTable();
  EXPECT_NE(op.Create(nullptr, nullptr), 0);

  CreateParam param{};
  param.cfg_file_name = "";
  void* handle = nullptr;
  EXPECT_NE(op.Create(&handle, &param), 0);

  NamedIoBatch inputs;
  NamedIoBatch outputs;
  EXPECT_NE(op.Process(nullptr, inputs, outputs), 0);
  EXPECT_NE(op.Control(nullptr, static_cast<int>(ControlCommand::kUpdateRules),
                       nullptr),
            0);
  EXPECT_NE(op.Destroy(nullptr), 0);
}

// 2. 测试句柄快速创建与销毁循环 (50轮生命周期与资源泄露检测)
TEST_F(OperatorSafetyTest, HandleLifecycleStressCycles50) {
  auto op = Get_LLM_EDGEFLOW_OperatorTable();
  CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kCpu;
  param.max_frame_depth = 25;

  for (int cycle = 0; cycle < 50; ++cycle) {
    void* handle = nullptr;
    int ret = op.Create(&handle, &param);
    ASSERT_EQ(ret, 0) << "Failed to create handle at cycle " << cycle;
    ASSERT_NE(handle, nullptr);

    ret = op.Destroy(handle);
    EXPECT_EQ(ret, 0) << "Failed to destroy handle at cycle " << cycle;
  }
}

// 3. 测试通过 Operator 接口全流程调用与动态控制规则生效
TEST_F(OperatorSafetyTest, EndToEndDynamicControlAndVerification) {
  auto op = Get_LLM_EDGEFLOW_OperatorTable();
  CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kCpu;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  int ret = op.Create(&handle, &param);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(handle, nullptr);

  // 下发动态规则
  ControlUpdateRulesParam ctrl{"{\"categories\": {\"TEST_VIP\": [\"VIP\"]}}"};
  ret =
      op.Control(handle, static_cast<int>(ControlCommand::kUpdateRules), &ctrl);
  EXPECT_EQ(ret, 0);

  // 执行推理
  std::string s0 = "请联系VIP专员";
  std::string s1 = "普通闲聊文本";
  CompanyString cs0{static_cast<int32_t>(s0.size()),
                    const_cast<char*>(s0.data())};
  CompanyString cs1{static_cast<int32_t>(s1.size()),
                    const_cast<char*>(s1.data())};

  CompanyOperatorKeywordInput req0{kMockServiceKeywordMatch, &cs0};
  CompanyOperatorKeywordInput req1{kMockServiceKeywordMatch, &cs1};

  NamedIoBatch inputs(2);
  inputs[0]["client_channel.keyword_in"] = MakeBorrowedOperatorInput(&req0);
  inputs[1]["client_channel.keyword_in"] = MakeBorrowedOperatorInput(&req1);

  NamedIoBatch outputs(2);
  outputs[0]["client_channel.keyword_out"] = nullptr;
  outputs[1]["client_channel.keyword_out"] = nullptr;

  ret = op.Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);

  auto out0_sp = outputs[0]["client_channel.keyword_out"];
  auto out1_sp = outputs[1]["client_channel.keyword_out"];
  ASSERT_NE(out0_sp, nullptr);
  ASSERT_NE(out1_sp, nullptr);

  auto* out0 = static_cast<CompanyOperatorKeywordOutput*>(out0_sp.get());
  auto* out1 = static_cast<CompanyOperatorKeywordOutput*>(out1_sp.get());

  EXPECT_EQ(out0->is_hit, 1);
  EXPECT_EQ(out1->is_hit, 0);
  ASSERT_NE(out0->match_result_json, nullptr);
  EXPECT_TRUE(std::string(out0->match_result_json->data,
                          out0->match_result_json->length)
                  .find("TEST_VIP") != std::string::npos);

  out0_sp.reset();
  out1_sp.reset();
  outputs.clear();
  ret = op.Destroy(handle);
  EXPECT_EQ(ret, 0);
}

// 4. 测试输出批次数量不匹配拦截契约
TEST_F(OperatorSafetyTest, OutputBatchSizeMismatchProtection) {
  auto op = Get_LLM_EDGEFLOW_OperatorTable();
  CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kCpu;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(op.Create(&handle, &param), 0);

  std::string s0 = "请联系VIP专员";
  std::string s1 = "普通闲聊文本";
  CompanyString cs0{static_cast<int32_t>(s0.size()),
                    const_cast<char*>(s0.data())};
  CompanyString cs1{static_cast<int32_t>(s1.size()),
                    const_cast<char*>(s1.data())};

  CompanyOperatorKeywordInput req0{kMockServiceKeywordMatch, &cs0};
  CompanyOperatorKeywordInput req1{kMockServiceKeywordMatch, &cs1};

  NamedIoBatch inputs(2);
  inputs[0]["client_channel.keyword_in"] = MakeBorrowedOperatorInput(&req0);
  inputs[1]["client_channel.keyword_in"] = MakeBorrowedOperatorInput(&req1);

  // 1) outputs 为空，批大小不匹配，必须返回错误
  NamedIoBatch empty_outputs;
  int ret = op.Process(handle, inputs, empty_outputs);
  EXPECT_NE(ret, 0);

  // 2) outputs 大小为 1 (小于需要的 2)，必须返回错误
  NamedIoBatch outputs(1);
  outputs[0]["client_channel.keyword_out"] = nullptr;
  ret = op.Process(handle, inputs, outputs);
  EXPECT_NE(ret, 0);

  EXPECT_EQ(op.Destroy(handle), 0);
}

// 5. 测试输入包含缺失槽位确定性拦截
TEST_F(OperatorSafetyTest, NullOrMissingSlotInBatchInputs) {
  auto op = Get_LLM_EDGEFLOW_OperatorTable();
  CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kCpu;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(op.Create(&handle, &param), 0);

  std::string s0 = "请联系VIP专员";
  CompanyString cs0{static_cast<int32_t>(s0.size()),
                    const_cast<char*>(s0.data())};
  CompanyOperatorKeywordInput req0{kMockServiceKeywordMatch, &cs0};

  NamedIoBatch inputs_with_missing_slot(2);
  inputs_with_missing_slot[0]["client_channel.keyword_in"] =
      MakeBorrowedOperatorInput(&req0);
  // 第二个样本缺少必需槽位 client_channel.keyword_in

  NamedIoBatch outputs(2);
  outputs[0]["client_channel.keyword_out"] = nullptr;
  outputs[1]["client_channel.keyword_out"] = nullptr;

  int ret = op.Process(handle, inputs_with_missing_slot, outputs);
  EXPECT_NE(ret, 0);

  EXPECT_EQ(op.Destroy(handle), 0);
}

TEST_F(OperatorSafetyTest, InputErrorsPrecedeOutputErrorsAndDoNotPublish) {
  auto op = Get_LLM_EDGEFLOW_OperatorTable();
  CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.compute_platform = ComputePlatform::kCpu;
  param.max_frame_depth = 1;
  void* raw_handle = nullptr;
  ASSERT_EQ(op.Create(&raw_handle, &param), 0);
  std::unique_ptr<void, int (*)(void*)> handle(raw_handle, op.Destroy);

  CompanyString text{1, nullptr};
  CompanyOperatorKeywordInput request{kMockServiceKeywordMatch, &text};
  NamedIoBatch inputs(1);
  inputs[0]["client.keyword_in"] = MakeBorrowedOperatorInput(&request);
  NamedIoBatch outputs(1);
  outputs[0]["invalid_output_key"] = nullptr;

  // 两侧均非法：载体校验必须优先于输出键解析。
  EXPECT_EQ(op.Process(handle.get(), inputs, outputs),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_NE(std::string(GetOperatorLastError())
                .find("Validation failed for input key client.keyword_in"),
            std::string::npos);
  ASSERT_EQ(outputs[0].size(), 1U);
  EXPECT_EQ(outputs[0].at("invalid_output_key"), nullptr);

  char valid_text[] = "ordinary request";
  text = {static_cast<int32_t>(std::strlen(valid_text)), valid_text};
  EXPECT_EQ(op.Process(handle.get(), inputs, outputs),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(std::string(GetOperatorLastError()),
            "Invalid key format in frame 0: invalid_output_key");
  EXPECT_EQ(outputs[0].at("invalid_output_key"), nullptr);

  outputs[0].clear();
  outputs[0]["client.keyword_out"] = nullptr;
  ASSERT_EQ(op.Process(handle.get(), inputs, outputs), COMPANY_ALG_SUCCESS);
  auto* result = static_cast<CompanyOperatorKeywordOutput*>(
      outputs[0].at("client.keyword_out").get());
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->status_code, 0);
}

TEST_F(OperatorSafetyTest,
       KeywordEncodeFailureRollsBackWholeBatchAndPoolLeases) {
  auto op = Get_LLM_EDGEFLOW_OperatorTable();
  CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.compute_platform = ComputePlatform::kCpu;
  param.max_frame_depth = 2;
  void* raw_handle = nullptr;
  ASSERT_EQ(op.Create(&raw_handle, &param), 0);
  std::unique_ptr<void, int (*)(void*)> handle(raw_handle, op.Destroy);

  // 只有第二个样本命中的类别序列化后超过配置的 2047 字节容量，
  // 第一个样本可以正常编码。
  const std::string rules =
      "{\"categories\":{\"" + std::string(2100, 'x') + "\":[\"overflow\"]}}";
  ControlUpdateRulesParam control{rules.c_str()};
  ASSERT_EQ(
      op.Control(handle.get(), static_cast<int>(ControlCommand::kUpdateRules),
                 &control),
      0);
  char ordinary[] = "ordinary";
  char overflow[] = "overflow";
  CompanyString first_text{8, ordinary};
  CompanyString second_text{8, overflow};
  CompanyOperatorKeywordInput first{kMockServiceKeywordMatch, &first_text};
  CompanyOperatorKeywordInput second{kMockServiceKeywordMatch, &second_text};
  NamedIoBatch inputs(2);
  inputs[0]["client.keyword_in"] = MakeBorrowedOperatorInput(&first);
  inputs[1]["client.keyword_in"] = MakeBorrowedOperatorInput(&second);
  NamedIoBatch outputs(2);
  for (auto& output : outputs) output["client.keyword_out"] = nullptr;

  EXPECT_EQ(op.Process(handle.get(), inputs, outputs),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_NE(std::string(GetOperatorLastError()).find("match_result_json"),
            std::string::npos);
  for (const auto& output : outputs) {
    ASSERT_EQ(output.size(), 1U);
    EXPECT_EQ(output.at("client.keyword_out"), nullptr);
  }

  // 用深度为 2 的池再处理一个两条目批次，证明所有失败的租约都已归还，
  // 包括已编码的第一个样本。
  second.sentence_text = &first_text;
  ASSERT_EQ(op.Process(handle.get(), inputs, outputs), COMPANY_ALG_SUCCESS);
  for (size_t i = 0; i < outputs.size(); ++i) {
    auto* result = static_cast<CompanyOperatorKeywordOutput*>(
        outputs[i].at("client.keyword_out").get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->is_hit, 0);
  }
}

// 7. 测试 RuntimeOptions 与设备参数贯通
TEST_F(OperatorSafetyTest, RuntimeOptionsAndDevicePropagation) {
  auto op = Get_LLM_EDGEFLOW_OperatorTable();
  CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.compute_platform = ComputePlatform::kCpu;
  param.device_id = 0;  // 显式指定设备 0

  void* handle0 = nullptr;
  ASSERT_EQ(op.Create(&handle0, &param), 0);
  EXPECT_EQ(op.Destroy(handle0), 0);

  param.device_id = 1;  // 显式指定设备 1
  void* handle1 = nullptr;
  ASSERT_EQ(op.Create(&handle1, &param), 0);
  EXPECT_EQ(op.Destroy(handle1), 0);
}

// 10. 测试有效批次上限契约强制执行
TEST_F(OperatorSafetyTest, FrameworkProcessBatchLimitEnforcement) {
  auto op = Get_LLM_EDGEFLOW_OperatorTable();
  CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kCpu;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(op.Create(&handle, &param), 0);

  // 构造 65 条输入数据，超过框架的 64 条上限
  std::string s = "测试输入";
  CompanyString cs{static_cast<int32_t>(s.size()), const_cast<char*>(s.data())};
  std::vector<CompanyOperatorKeywordInput> reqs(65);
  NamedIoBatch inputs(65);
  NamedIoBatch outputs(65);
  for (int i = 0; i < 65; ++i) {
    reqs[i].service_type = kMockServiceKeywordMatch;
    reqs[i].sentence_text = &cs;
    inputs[i]["client_channel.keyword_in"] =
        MakeBorrowedOperatorInput(&reqs[i]);
    outputs[i]["client_channel.keyword_out"] = nullptr;
  }

  // 超过框架批次上限时必须前置拦截并返回错误
  int ret = op.Process(handle, inputs, outputs);
  EXPECT_NE(ret, 0);

  EXPECT_EQ(op.Destroy(handle), 0);
}

// 11. 同一 handle 的并发 Process 由接入适配层串行化，停流 join 后才允许 Destroy
TEST_F(OperatorSafetyTest, SameHandleConcurrentProcessAndQuiescedDestroy) {
  auto op = Get_LLM_EDGEFLOW_OperatorTable();
  CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kCpu;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(op.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  constexpr int kThreadCount = 8;
  constexpr int kCallsPerThread = 40;
  std::atomic<bool> start{false};
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreadCount);

  for (int thread_index = 0; thread_index < kThreadCount; ++thread_index) {
    workers.emplace_back([&, thread_index]() {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (int call_index = 0; call_index < kCallsPerThread; ++call_index) {
        std::string s = "same handle request";
        CompanyString cs{static_cast<int32_t>(s.size()),
                         const_cast<char*>(s.data())};
        CompanyOperatorKeywordInput input{kMockServiceKeywordMatch, &cs};

        NamedIoBatch inputs(1);
        inputs[0]["client_channel.keyword_in"] =
            MakeBorrowedOperatorInput(&input);
        NamedIoBatch outputs(1);
        outputs[0]["client_channel.keyword_out"] = nullptr;

        const int ret = op.Process(handle, inputs, outputs);
        if (ret != 0) {
          failures.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        auto out_sp = outputs[0]["client_channel.keyword_out"];
        if (!out_sp) {
          failures.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        auto* out_dto =
            static_cast<CompanyOperatorKeywordOutput*>(out_sp.get());
        if (out_dto->status_code != 0 || !out_dto->match_result_json) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        outputs.clear();
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (auto& worker : workers) {
    worker.join();
  }

  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(op.Destroy(handle), 0);
}

// 12. Entity 失败样本在结构化校验失败时，业务字段和 status
// 与 entities_json 保留原调用者哨兵值
TEST_F(OperatorSafetyTest, EntityFailureSampleSentinelValues) {
  const auto* out_conv =
      llm_edgeflow::IoConverterRegistry::Instance().FindOutputConverter(
          "entity_out", "entity_extract");
  ASSERT_NE(out_conv, nullptr);

  llm_edgeflow::AlgContext ctx;

  llm_edgeflow::StructuredDocumentBatch entities;
  entities.emplace_back(
      0, 0,
      llm_edgeflow::JsonDocumentItem("[\"valid_entity\"]", true,
                                     llm_edgeflow::JsonParseStatus::kOk));
  entities.emplace_back(
      1, 0,
      llm_edgeflow::JsonDocumentItem("invalid", false,
                                     llm_edgeflow::JsonParseStatus::kFailed));
  ctx.Publish(llm_edgeflow::kEntities, std::move(entities));

  char buf0[512] = {0};
  char buf1[512] = {0};
  std::strcpy(buf1, "SENTINEL_PAYLOAD");
  CompanyString cs0{0, buf0};
  CompanyString cs1{static_cast<int32_t>(std::strlen("SENTINEL_PAYLOAD")),
                    buf1};

  CompanyOperatorEntityOutput out0{}, out1{};
  out0.entities_json = &cs0;
  out1.entities_json = &cs1;
  out1.status_code = -777;

  llm_edgeflow::TestOutputBatchView out_view;
  out_view.count = 2;
  out_view.leased_slots["entity_out"] = {&out0, &out1};
  out_view.slot_types["entity_out"] = "CompanyOperatorEntityOutput";
  out_view.SetCapacity("entity_out", "entities_json", 511);

  llm_edgeflow::OutputEncodeOptions options;
  const llm_edgeflow::IoPortBindings options_ports =
      llm_edgeflow::test::ConverterPortsForTest(*out_conv);
  options.ports = &options_ports;

  options.type = out_conv->type;
  options.name = out_conv->name;

  size_t written_count = 0;
  llm_edgeflow::AdapterStatus status;
  int ret = ::llm_edgeflow::test::EncodeForTest(
      *out_conv, &ctx, options, &out_view, &written_count, &status);

  EXPECT_EQ(ret, COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(out0.status_code, 0);
  EXPECT_STREQ(out0.entities_json->data, "[\"valid_entity\"]");

  // 样本 1：业务校验失败，binding 未写入，所有平台字段保持哨兵值。
  EXPECT_EQ(out1.status_code, -777);
  EXPECT_STREQ(out1.entities_json->data, "SENTINEL_PAYLOAD");
}

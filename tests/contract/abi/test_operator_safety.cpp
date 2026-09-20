#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "adapter/biz_blackboard_keys.h"
#include "adapter/biz_results.h"
#include "adapter/io_binding_registry.h"
#include "adapter/io_converter_registry.h"
#include "edgeflow/operator/interface.h"
#include "edgeflow/operator/types.h"
#include "platform_mock/error_codes.h"
#include "tests/support/adapter_test_views.h"

using namespace llm_edgeflow::operator_api;

class OperatorSafetyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    llm_edgeflow::IoBindingRegistry::Instance().ResetConflictForTesting();
    llm_edgeflow::IoConverterRegistry::Instance().ResetConflictForTesting();
    Get_LLM_EDGEFLOW_OperatorTable().Init();
  }
  void TearDown() override {
    Get_LLM_EDGEFLOW_OperatorTable().Deinit();
    llm_edgeflow::IoBindingRegistry::Instance().ResetConflictForTesting();
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
  EXPECT_NE(op.Control(nullptr, ControlCommand::kUpdateRules, nullptr), 0);
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
  ret = op.Control(handle, ControlCommand::kUpdateRules, &ctrl);
  EXPECT_EQ(ret, 0);

  // 执行推理
  std::string s0 = "请联系VIP专员";
  std::string s1 = "普通闲聊文本";
  CompanyString cs0{static_cast<int32_t>(s0.size()),
                    const_cast<char*>(s0.data())};
  CompanyString cs1{static_cast<int32_t>(s1.size()),
                    const_cast<char*>(s1.data())};

  CompanyOperatorKeywordInput req0{101, &cs0};
  CompanyOperatorKeywordInput req1{102, &cs1};

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

  CompanyOperatorKeywordInput req0{101, &cs0};
  CompanyOperatorKeywordInput req1{102, &cs1};

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
  CompanyOperatorKeywordInput req0{101, &cs0};

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

// 6. 测试 IoBinding 注册冲突防护与定义机器可读性
TEST_F(OperatorSafetyTest, IoBindingRegistryConflictDetectionAndDescriptor) {
  auto& registry = llm_edgeflow::IoBindingRegistry::Instance();
  const auto* binding = registry.FindBinding("keyword_match.operator.v1");
  ASSERT_NE(binding, nullptr);

  EXPECT_EQ(binding->binding_id, "keyword_match.operator.v1");
  EXPECT_EQ(binding->biz_name, "keyword_match_v1");

  EXPECT_GT(binding->max_batch_size, 0);

  // 测试重复 binding 注册拦截
  bool reg_dup_ret = registry.RegisterBinding(*binding);
  EXPECT_FALSE(reg_dup_ret) << "Duplicate binding_id registration must fail";
  registry.ResetConflictForTesting();
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

// 8. 测试配置中未知/缺失 binding 在 Create 前置拦截
TEST_F(OperatorSafetyTest, UnknownAndUnregisteredBindingRejectionInCreate) {
  auto op = Get_LLM_EDGEFLOW_OperatorTable();

  // 1) 传入不存在的接入配置
  CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "non_existent_config.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kCpu;
  void* handle = nullptr;
  int ret = op.Create(&handle, &param);
  EXPECT_NE(ret, 0);
  EXPECT_EQ(handle, nullptr);

  // 2) 传入缺失 io_binding 的配置
  std::string bad_cfg = "./results/test_missing_binding.conf";
  std::filesystem::create_directories("./results");
  {
    std::ofstream ofs(bad_cfg);
    ofs << R"({"schema_version": 1, "data": {"pipe_path": "pipeline_keyword_match_rules.json"}})";
  }
  param.cfg_file_name = bad_cfg.c_str();
  ret = op.Create(&handle, &param);
  EXPECT_NE(ret, 0);
  EXPECT_EQ(handle, nullptr);

  // 3) 传入未知 io_binding
  {
    std::ofstream ofs(bad_cfg);
    ofs << R"({"schema_version": 1, "data": {"pipe_path": "pipeline_keyword_match_rules.json", "io_binding": "unknown.binding.v999"}})";
  }
  ret = op.Create(&handle, &param);
  EXPECT_NE(ret, 0);
  EXPECT_EQ(handle, nullptr);

  // 4) 传入旧 cabi 绑定
  {
    std::ofstream ofs(bad_cfg);
    ofs << R"({"schema_version": 1, "data": {"pipe_path": "pipeline_keyword_match_rules.json", "io_binding": "keyword_match.cabi.v1"}})";
  }
  ret = op.Create(&handle, &param);
  EXPECT_NE(ret, 0);
  EXPECT_EQ(handle, nullptr);

  std::filesystem::remove(bad_cfg);
}

// 9. 测试 Registry 冲突 fail-closed 导致 Init 失败
TEST_F(OperatorSafetyTest, FailClosedRegistryConflictAndInitFailure) {
  auto& registry = llm_edgeflow::IoBindingRegistry::Instance();
  registry.ResetConflictForTesting();
  auto op = Get_LLM_EDGEFLOW_OperatorTable();

  // 初始干净状态 Init 成功
  EXPECT_EQ(op.Init(), 0);

  // 注册冲突（重复注册 binding）
  const auto* binding = registry.FindBinding("keyword_match.operator.v1");
  ASSERT_NE(binding, nullptr);
  bool reg_ret = registry.RegisterBinding(*binding);
  EXPECT_FALSE(reg_ret);
  EXPECT_TRUE(registry.HasConflict());

  // 注册冲突发生后，Init 必须 fail-closed 返回 -6
  EXPECT_EQ(op.Init(), -6);

  // 测试结束后清理恢复干净状态
  registry.ResetConflictForTesting();
  EXPECT_FALSE(registry.HasConflict());
  EXPECT_EQ(op.Init(), 0);
}

// 10. 测试有效批次上限契约强制执行
TEST_F(OperatorSafetyTest, AdapterDescriptorMaxBatchSizeEnforcement) {
  auto op = Get_LLM_EDGEFLOW_OperatorTable();
  CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = ComputePlatform::kCpu;
  param.max_frame_depth = 25;

  void* handle = nullptr;
  ASSERT_EQ(op.Create(&handle, &param), 0);

  // 构造 65 条输入数据 (超过 max_batch_size = 64 上限)
  std::string s = "测试输入";
  CompanyString cs{static_cast<int32_t>(s.size()), const_cast<char*>(s.data())};
  std::vector<CompanyOperatorKeywordInput> reqs(65);
  NamedIoBatch inputs(65);
  NamedIoBatch outputs(65);
  for (int i = 0; i < 65; ++i) {
    reqs[i].request_id = i + 1;
    reqs[i].sentence_text = &cs;
    inputs[i]["client_channel.keyword_in"] =
        MakeBorrowedOperatorInput(&reqs[i]);
    outputs[i]["client_channel.keyword_out"] = nullptr;
  }

  // 超过 max_batch_size -> 必须被前置拦截返回错误
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
        const uint64_t request_id =
            static_cast<uint64_t>(thread_index * kCallsPerThread + call_index);
        std::string s = "same handle request";
        CompanyString cs{static_cast<int32_t>(s.size()),
                         const_cast<char*>(s.data())};
        CompanyOperatorKeywordInput input{request_id, &cs};

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
        if (out_dto->request_id != request_id) {
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

// 12. RFC-0053: Entity 失败样本在结构化校验失败时，先写 request_id，但 status
// 与 entities_json 保留原调用者哨兵值
TEST_F(OperatorSafetyTest, EntityFailureSampleSentinelValues) {
  const auto* out_conv =
      llm_edgeflow::IoConverterRegistry::Instance().FindOutputConverter(
          "document.structured.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  llm_edgeflow::AlgContext ctx;
  ctx.Publish(llm_edgeflow::kRawRequestIds, std::vector<uint64_t>{1001, 2002});

  llm_edgeflow::StructuredDocumentBatch entities;
  entities.emplace_back(
      0, 0,
      llm_edgeflow::JsonDocumentItem("[\"valid_entity\"]", true,
                                     llm_edgeflow::JsonParseStatus::kOk));
  entities.emplace_back(
      1, 0,
      llm_edgeflow::JsonDocumentItem("invalid", false,
                                     llm_edgeflow::JsonParseStatus::kFailed));
  ctx.Publish(llm_edgeflow::kExtractedEntities, std::move(entities));

  char buf0[512] = {0};
  char buf1[512] = {0};
  std::strcpy(buf1, "SENTINEL_PAYLOAD");
  CompanyString cs0{0, buf0};
  CompanyString cs1{static_cast<int32_t>(std::strlen("SENTINEL_PAYLOAD")),
                    buf1};

  CompanyOperatorEntityOutput out0{}, out1{};
  out0.entities_json = &cs0;
  out1.entities_json = &cs1;
  out1.request_id = 99999;
  out1.status_code = -777;

  llm_edgeflow::TestOutputBatchView out_view;
  out_view.count = 2;
  out_view.leased_slots["entity_out"] = {&out0, &out1};
  out_view.slot_types["entity_out"] = "CompanyOperatorEntityOutput";
  out_view.SetCapacity("entity_out", "entities_json", 511);

  llm_edgeflow::OutputEncodeOptions options;

  options.converter_id = out_conv->converter_id;

  llm_edgeflow::OutputPortBindings bindings(
      {{"raw_request_ids", "raw_request_ids"},
       {"extracted_entities", "extracted_entities"}});
  size_t written_count = 0;
  llm_edgeflow::AdapterStatus status;
  int ret = out_conv->encode_fn(&ctx, bindings, options, &out_view,
                                &written_count, &status);

  EXPECT_EQ(ret, COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(out0.request_id, 1001u);
  EXPECT_EQ(out0.status_code, 0);
  EXPECT_STREQ(out0.entities_json->data, "[\"valid_entity\"]");

  // Sample 1: request_id was written, but status_code and entities_json
  // retained sentinels
  EXPECT_EQ(out1.request_id, 2002u);
  EXPECT_EQ(out1.status_code, -777);
  EXPECT_STREQ(out1.entities_json->data, "SENTINEL_PAYLOAD");
}

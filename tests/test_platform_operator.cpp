#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "adapter/platform/platform_io_registry.h"
#include "company_alg_interface.h"
#include "platform/platform_operator_interface.h"

using namespace llm_edgeflow::platform;

static std::string GetConfPath(const std::string& rel_path) {
  FILE* fp = fopen(rel_path.c_str(), "r");
  if (fp) {
    fclose(fp);
    return rel_path;
  }
  return "../" + rel_path;
}

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
  std::string valid_conf = GetConfPath("configs/pipeline_keyword_match.conf");

  // 1. 空 handle 指针
  EXPECT_EQ(ops_.Create(nullptr, nullptr), -1);

  // 2. *handle 非空
  void* dummy_ptr = reinterpret_cast<void*>(0x1234);
  CreateParam param{};
  param.cfg_file_name = valid_conf.c_str();
  param.platform_config.batch_size = 1;
  param.platform_config.device_id = 0;
  param.platform_config.type = ChipType::kAx650;
  param.depth_num = 1;
  EXPECT_EQ(ops_.Create(&dummy_ptr, &param), -1);

  // 3. 空 param
  handle = nullptr;
  EXPECT_EQ(ops_.Create(&handle, nullptr), -1);

  // 4. 空配置路径
  param.cfg_file_name = nullptr;
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  param.cfg_file_name = "";
  EXPECT_EQ(ops_.Create(&handle, &param), -2);

  // 5. 非法 batch_size <= 0
  param.cfg_file_name = valid_conf.c_str();
  param.platform_config.batch_size = 0;
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  param.platform_config.batch_size = -1;
  EXPECT_EQ(ops_.Create(&handle, &param), -2);

  // 6. 非法 device_id < 0
  param.platform_config.batch_size = 1;
  param.platform_config.device_id = -1;
  EXPECT_EQ(ops_.Create(&handle, &param), -2);

  // 7. 未知芯片类型 ChipType::kUnknown 及非法枚举值
  param.platform_config.device_id = 0;
  param.platform_config.type = ChipType::kUnknown;
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  param.platform_config.type = static_cast<ChipType>(9999);
  EXPECT_EQ(ops_.Create(&handle, &param), -2);

  // 8. 非法 depth_num == 0
  param.platform_config.type = ChipType::kAx650;
  param.depth_num = 0;
  EXPECT_EQ(ops_.Create(&handle, &param), -2);

  // 9. 文件不存在
  param.depth_num = 1;
  param.cfg_file_name = "non_existent_file.conf";
  EXPECT_EQ(ops_.Create(&handle, &param), -2);
  EXPECT_NE(GetPlatformLastError(), nullptr);
}

// 3. 强类型 Control 正常与边界异常测试 (含 NaN / Infinity 拦截，P2-1 修复验证)
TEST_F(PlatformOperatorTest, StronglyTypedControlValidation) {
  std::string conf_path = GetConfPath("configs/pipeline_keyword_match.conf");
  CreateParam param{};
  param.cfg_file_name = conf_path.c_str();
  param.platform_config.batch_size = 2;
  param.platform_config.device_id = 0;
  param.platform_config.type = ChipType::kAx650;
  param.depth_num = 1;

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

  // NaN 拦截
  float nan_val = std::numeric_limits<float>::quiet_NaN();
  ControlUpdateThresholdParam thresh_nan{"VIP_SERVICE", nan_val};
  EXPECT_EQ(ops_.Control(handle, ControlCommand::kUpdateThreshold, &thresh_nan),
            -2);

  // Infinity 拦截
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

// 4. 活跃句柄注册中心与 UAF 防护测试 (P0-2 修复验证)
TEST_F(PlatformOperatorTest, HandleLifecycleAndUafPrevention) {
  std::string conf_path = GetConfPath("configs/pipeline_keyword_match.conf");
  CreateParam param{};
  param.cfg_file_name = conf_path.c_str();
  param.platform_config.batch_size = 2;
  param.platform_config.device_id = 0;
  param.platform_config.type = ChipType::kAx650;
  param.depth_num = 1;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  // 1. 正常单次销毁
  EXPECT_EQ(ops_.Destroy(handle), 0);

  // 2. 重复销毁 (Double Destroy): 安全返回 -1，绝不发生 UAF / SIGSEGV
  EXPECT_EQ(ops_.Destroy(handle), -1);

  // 3. 销毁后调用 Process / Control: 安全返回 -1
  CompanyKeywordInputStruct in{101, "test"};
  CompanyKeywordOutputStruct out{};
  NamedIoBatch in_b(1), out_b(1);
  in_b[0]["chan.keyword_in"] = std::shared_ptr<void>(&in, [](void*) {});
  out_b[0]["chan.keyword_out"] = std::shared_ptr<void>(&out, [](void*) {});

  EXPECT_EQ(ops_.Process(handle, in_b, out_b), -1);
  ControlUpdateRulesParam ctrl_param{"{}"};
  EXPECT_EQ(ops_.Control(handle, ControlCommand::kUpdateRules, &ctrl_param),
            -1);

  // 4. 传入随机垃圾地址指针: 注册表中查不到，安全返回 -1
  void* fake_handle = reinterpret_cast<void*>(0xdeadbeef);
  EXPECT_EQ(ops_.Destroy(fake_handle), -1);
  EXPECT_EQ(ops_.Process(fake_handle, in_b, out_b), -1);
  EXPECT_EQ(
      ops_.Control(fake_handle, ControlCommand::kUpdateRules, &ctrl_param), -1);
}

// 5. 验证 depth_num 预分配 Hook 正常生命周期与失败回滚 (P1-3 修复验证)
TEST_F(PlatformOperatorTest, DepthNumHookAndRollback) {
  std::string conf_path = GetConfPath("configs/pipeline_keyword_match.conf");

  // 5.1 正常分配与销毁
  static std::atomic<int> alloc_count{0};
  static std::atomic<int> dealloc_count{0};
  alloc_count.store(0);
  dealloc_count.store(0);

  auto my_allocator = [](const char* slot,
                         void* user_data) -> std::shared_ptr<void> {
    (void)slot;
    (void)user_data;
    alloc_count.fetch_add(1);
    return std::make_shared<CompanyKeywordOutputStruct>();
  };

  auto my_deallocator = [](const char* slot, std::shared_ptr<void> ptr,
                           void* user_data) {
    (void)slot;
    (void)ptr;
    (void)user_data;
    dealloc_count.fetch_add(1);
  };

  CreateParam param{};
  param.cfg_file_name = conf_path.c_str();
  param.platform_config.batch_size = 2;
  param.platform_config.device_id = 0;
  param.platform_config.type = ChipType::kAx650;
  param.depth_num = 4;
  param.output_allocator = my_allocator;
  param.output_deallocator = my_deallocator;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  EXPECT_EQ(alloc_count.load(), 4);

  ASSERT_EQ(ops_.Destroy(handle), 0);
  EXPECT_EQ(dealloc_count.load(), 4);

  // 5.2 分配中途失败真实回滚验证 (P2-3 修复验证)
  static std::atomic<int> fail_alloc_count{0};
  static std::atomic<int> fail_dealloc_count{0};
  fail_alloc_count.store(0);
  fail_dealloc_count.store(0);

  auto failing_allocator = [](const char* slot,
                              void* user_data) -> std::shared_ptr<void> {
    (void)slot;
    (void)user_data;
    int cur = fail_alloc_count.fetch_add(1);
    if (cur >= 2) {
      return nullptr;  // 第 3 次分配时故意返回空指针，触发回滚
    }
    return std::make_shared<CompanyKeywordOutputStruct>();
  };

  auto failing_deallocator = [](const char* slot, std::shared_ptr<void> ptr,
                                void* user_data) {
    (void)slot;
    (void)ptr;
    (void)user_data;
    fail_dealloc_count.fetch_add(1);
  };

  param.depth_num = 4;
  param.output_allocator = failing_allocator;
  param.output_deallocator = failing_deallocator;
  void* fail_handle = nullptr;

  int ret = ops_.Create(&fail_handle, &param);
  EXPECT_EQ(ret, -4);  // 内存分配失败返回 -4
  EXPECT_EQ(fail_handle, nullptr);
  // 验证之前成功分配的 2 个对象被全量回滚释放
  EXPECT_EQ(fail_dealloc_count.load(), 2);
}

// 6. 验证 Deinit 托管清理活跃句柄与输出池 (P1-2 修复验证)
TEST_F(PlatformOperatorTest, DeinitCleansActiveHandlesAndPools) {
  std::string conf_path = GetConfPath("configs/pipeline_keyword_match.conf");

  static std::atomic<int> active_dealloc_count{0};
  active_dealloc_count.store(0);

  auto dealloc = [](const char* slot, std::shared_ptr<void> ptr,
                    void* user_data) {
    (void)slot;
    (void)ptr;
    (void)user_data;
    active_dealloc_count.fetch_add(1);
  };

  auto alloc = [](const char* slot, void* user_data) -> std::shared_ptr<void> {
    (void)slot;
    (void)user_data;
    return std::make_shared<CompanyKeywordOutputStruct>();
  };

  CreateParam param{};
  param.cfg_file_name = conf_path.c_str();
  param.platform_config.batch_size = 2;
  param.platform_config.device_id = 0;
  param.platform_config.type = ChipType::kAx650;
  param.depth_num = 3;
  param.output_allocator = alloc;
  param.output_deallocator = dealloc;

  void* h1 = nullptr;
  void* h2 = nullptr;
  ASSERT_EQ(ops_.Create(&h1, &param), 0);
  ASSERT_EQ(ops_.Create(&h2, &param), 0);

  // 在没有显式 Destroy 的情况下直接调用 Deinit
  int deinit_ret = ops_.Deinit();
  EXPECT_EQ(deinit_ret, 0);

  // 验证 2 个句柄 * depth_num(3) = 6 个池化对象全部被安全回收
  EXPECT_EQ(active_dealloc_count.load(), 6);

  // 再次对已被托管清理的句柄调用 Destroy，安全返回 -1
  EXPECT_EQ(ops_.Destroy(h1), -1);
  EXPECT_EQ(ops_.Destroy(h2), -1);

  // 重新 Init 恢复测试环境
  ASSERT_EQ(ops_.Init(), 0);
}

// 7. 验证未被覆盖的相对模型路径自动绝对化 (P1-1 修复验证)
TEST_F(PlatformOperatorTest, UncoveredModelPathsNormalization) {
  // 使用多模型 DocQA pipeline，但 conf 中仅覆盖单个模型或零覆盖
  std::string conf_path = GetConfPath("configs/pipeline_doc_qa.conf");

  CreateParam param{};
  param.cfg_file_name = conf_path.c_str();
  param.platform_config.batch_size = 2;
  param.platform_config.device_id = 0;
  param.platform_config.type = ChipType::kAx650;
  param.depth_num = 1;

  void* handle = nullptr;
  int ret = ops_.Create(&handle, &param);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(handle, nullptr);

  ops_.Destroy(handle);
}

// 8. 验证纯 C ABI 遇到非法 Pipeline 配置构建失败时返回 -3 (P1-3 修复验证)
TEST_F(PlatformOperatorTest, PureCAbiConfigFailureErrorCode) {
  // 临时创建一个 business_name 合法但 node_type 未知的非法 pipeline
  std::string invalid_pipe = "temp_invalid_pipeline.json";
  {
    std::ofstream ofs(invalid_pipe);
    ofs << "{\n"
        << "  \"business_name\": \"keyword_match_v1\",\n"
        << "  \"models\": [],\n"
        << "  \"pipeline\": [{\"node_type\": \"UnknownNonExistentNode\"}]\n"
        << "}\n";
  }

  CompanyAlgParamCreate c_param{};
  c_param.config_file_path = invalid_pipe.c_str();
  c_param.model_root_dir = "";
  c_param.device_id = 0;
  c_param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

  void* c_handle = nullptr;
  int ret = Alg_Create(&c_handle, &c_param);
  // 按照 main 纯 C ABI V2 规范，构建失败返回 -3 (COMPANY_ALG_ERR_INVALID_INPUT)
  EXPECT_EQ(ret, -3);
  EXPECT_EQ(c_handle, nullptr);

  std::filesystem::remove(invalid_pipe);
}

// 9. 验证 PlatformIoRegistry 描述符不变量注册检查 (P2-2 修复验证)
TEST_F(PlatformOperatorTest, PlatformIoRegistryDescriptorInvariants) {
  // 1. 无效 BizType
  alg_framework::PlatformIoDescriptor invalid_desc{};
  invalid_desc.biz_type = ALG_BIZ_TYPE_UNKNOWN;
  invalid_desc.biz_name = "test";
  EXPECT_FALSE(alg_framework::PlatformIoRegistry::Instance().RegisterDescriptor(
      invalid_desc));

  // 2. 空槽位组
  invalid_desc.biz_type = static_cast<CompanyAlgBizType>(100);
  invalid_desc.biz_name = "test_empty_slots";
  EXPECT_FALSE(alg_framework::PlatformIoRegistry::Instance().RegisterDescriptor(
      invalid_desc));

  // 重置回健康状态，确保单测隔离
  alg_framework::PlatformIoRegistry::Instance().ResetForTesting();
}

// 10. 业务 1 (关注词匹配) 全生命周期与命名 I/O 执行
TEST_F(PlatformOperatorTest, KeywordMatchLifecycleAndExecution) {
  std::string conf_path = GetConfPath("configs/pipeline_keyword_match.conf");

  CreateParam param{};
  param.cfg_file_name = conf_path.c_str();
  param.platform_config.batch_size = 4;
  param.platform_config.device_id = 0;
  param.platform_config.type = ChipType::kAx650;
  param.depth_num = 2;

  void* handle = nullptr;
  int ret = ops_.Create(&handle, &param);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(handle, nullptr);

  // 动态控制：更新词表 (强类型结构体)
  ControlUpdateRulesParam rules_param{
      "{\n"
      "  \"categories\": {\n"
      "    \"VIP_SERVICE\": [\"VIP\", \"加急\", \"专员\"]\n"
      "  }\n"
      "}"};
  ret = ops_.Control(handle, ControlCommand::kUpdateRules, &rules_param);
  EXPECT_EQ(ret, 0);

  // 执行 Process: 命名 I/O (点后缀 key: "channel_0.keyword_in")
  CompanyKeywordInputStruct in0{101, "请帮我联系VIP客服专员加急办理"};
  CompanyKeywordInputStruct in1{102, "今天天气真好，去散步吧"};
  CompanyKeywordOutputStruct out0{};
  CompanyKeywordOutputStruct out1{};

  NamedIoBatch inputs(2);
  NamedIoBatch outputs(2);

  inputs[0]["client_channel.keyword_in"] =
      std::shared_ptr<void>(&in0, [](void*) {});
  inputs[1]["client_channel.keyword_in"] =
      std::shared_ptr<void>(&in1, [](void*) {});

  outputs[0]["client_channel.keyword_out"] =
      std::shared_ptr<void>(&out0, [](void*) {});
  outputs[1]["client_channel.keyword_out"] =
      std::shared_ptr<void>(&out1, [](void*) {});

  ret = ops_.Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);

  EXPECT_EQ(out0.request_id, 101);
  EXPECT_EQ(out0.is_hit, 1);
  EXPECT_EQ(out0.status_code, 0);

  EXPECT_EQ(out1.request_id, 102);
  EXPECT_EQ(out1.is_hit, 0);
  EXPECT_EQ(out1.status_code, 0);

  // 销毁句柄
  ret = ops_.Destroy(handle);
  EXPECT_EQ(ret, 0);
}

// 11. 命名 I/O 错误边界校验 (别名组、非法后缀、空指针)
TEST_F(PlatformOperatorTest, NamedIoErrorHandling) {
  std::string conf_path = GetConfPath("configs/pipeline_keyword_match.conf");

  CreateParam param{};
  param.cfg_file_name = conf_path.c_str();
  param.platform_config.batch_size = 2;
  param.platform_config.device_id = 0;
  param.platform_config.type = ChipType::kAx650;
  param.depth_num = 1;

  void* handle = nullptr;
  int ret = ops_.Create(&handle, &param);
  ASSERT_EQ(ret, 0);

  CompanyKeywordInputStruct in0{101, "test"};
  CompanyKeywordOutputStruct out0{};

  // 1. 空输入批
  NamedIoBatch empty_inputs;
  NamedIoBatch empty_outputs;
  EXPECT_EQ(ops_.Process(handle, empty_inputs, empty_outputs), -3);

  // 2. 输入输出批大小不匹配
  NamedIoBatch in_batch(2);
  NamedIoBatch out_batch(1);
  EXPECT_EQ(ops_.Process(handle, in_batch, out_batch), -3);

  // 3. 超过 batch_size 上限 (batch_size=2, 传入 3)
  NamedIoBatch in_batch3(3);
  NamedIoBatch out_batch3(3);
  EXPECT_EQ(ops_.Process(handle, in_batch3, out_batch3), -3);

  // 4. 无点号 Key 错误 (如 "keyword_in")
  NamedIoBatch in_bad(1);
  NamedIoBatch out_bad(1);
  in_bad[0]["invalid_key_without_dot"] =
      std::shared_ptr<void>(&in0, [](void*) {});
  out_bad[0]["channel.keyword_out"] =
      std::shared_ptr<void>(&out0, [](void*) {});
  EXPECT_EQ(ops_.Process(handle, in_bad, out_bad), -3);

  // 5. 未知后缀 (如 "channel.unknown_suffix")
  in_bad[0].clear();
  in_bad[0]["channel.unknown_suffix"] =
      std::shared_ptr<void>(&in0, [](void*) {});
  EXPECT_EQ(ops_.Process(handle, in_bad, out_bad), -3);

  // 6. 空 shared_ptr 指针
  in_bad[0].clear();
  in_bad[0]["channel.keyword_in"] = nullptr;
  EXPECT_EQ(ops_.Process(handle, in_bad, out_bad), -3);

  // 7. 重复槽位 (同时传入主名称和别名)
  in_bad[0].clear();
  in_bad[0]["channel.keyword_in"] = std::shared_ptr<void>(&in0, [](void*) {});
  in_bad[0]["channel.sentence_in"] = std::shared_ptr<void>(&in0, [](void*) {});
  EXPECT_EQ(ops_.Process(handle, in_bad, out_bad), -3);

  ops_.Destroy(handle);
}

// 12. 同句柄互斥与多句柄并发测试 (P2-5 修复验证)
TEST_F(PlatformOperatorTest, SameHandleMutualExclusionAndConcurrency) {
  std::string conf_path = GetConfPath("configs/pipeline_keyword_match.conf");

  CreateParam param{};
  param.cfg_file_name = conf_path.c_str();
  param.platform_config.batch_size = 2;
  param.platform_config.device_id = 0;
  param.platform_config.type = ChipType::kAx650;
  param.depth_num = 1;

  void* handle = nullptr;
  ASSERT_EQ(ops_.Create(&handle, &param), 0);
  ASSERT_NE(handle, nullptr);

  std::atomic<bool> stop_flag{false};
  std::atomic<int> process_count{0};
  std::atomic<int> control_count{0};

  // 线程 1: 持续发起 Process
  std::thread worker_process([this, handle, &stop_flag, &process_count]() {
    while (!stop_flag.load()) {
      CompanyKeywordInputStruct in{1001, "测试语句"};
      CompanyKeywordOutputStruct out{};
      NamedIoBatch in_b(1), out_b(1);
      in_b[0]["chan.keyword_in"] = std::shared_ptr<void>(&in, [](void*) {});
      out_b[0]["chan.keyword_out"] = std::shared_ptr<void>(&out, [](void*) {});
      int ret = ops_.Process(handle, in_b, out_b);
      EXPECT_EQ(ret, 0);
      process_count.fetch_add(1);
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  });

  // 线程 2: 持续发起 Control (同句柄互斥竞争)
  std::thread worker_control([this, handle, &stop_flag, &control_count]() {
    while (!stop_flag.load()) {
      ControlUpdateRulesParam rules{"{\"categories\":{\"TEST\":[\"测试\"]}}"};
      int ret = ops_.Control(handle, ControlCommand::kUpdateRules, &rules);
      EXPECT_EQ(ret, 0);
      control_count.fetch_add(1);
      std::this_thread::sleep_for(std::chrono::microseconds(150));
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stop_flag.store(true);

  worker_process.join();
  worker_control.join();

  EXPECT_GT(process_count.load(), 0);
  EXPECT_GT(control_count.load(), 0);

  ops_.Destroy(handle);
}

// 13. 业务 2 (实体/名词提取) 平台 Operator 测试
TEST_F(PlatformOperatorTest, EntityExtractPipeline) {
  std::string conf_path = GetConfPath("configs/pipeline_entity_extract.conf");

  CreateParam param{};
  param.cfg_file_name = conf_path.c_str();
  param.platform_config.batch_size = 2;
  param.platform_config.device_id = 0;
  param.platform_config.type = ChipType::kAx650;
  param.depth_num = 1;

  void* handle = nullptr;
  int ret = ops_.Create(&handle, &param);
  ASSERT_EQ(ret, 0);

  CompanyEntityInputStruct in0{201,
                               "清华大学的张三加入了一家北京的人工智能公司"};
  CompanyEntityOutputStruct out0{};

  NamedIoBatch inputs(1);
  NamedIoBatch outputs(1);
  inputs[0]["nlp_node.entity_in"] = std::shared_ptr<void>(&in0, [](void*) {});
  outputs[0]["nlp_node.entity_out"] =
      std::shared_ptr<void>(&out0, [](void*) {});

  ret = ops_.Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out0.request_id, 201);
  EXPECT_TRUE(strlen(out0.entities_json) > 0);

  ops_.Destroy(handle);
}

// 14. 业务 3 (智能长文档切片问答 RAG) 平台 Operator 测试
TEST_F(PlatformOperatorTest, DocQaPipeline) {
  std::string conf_path = GetConfPath("configs/pipeline_doc_qa.conf");

  CreateParam param{};
  param.cfg_file_name = conf_path.c_str();
  param.platform_config.batch_size = 2;
  param.platform_config.device_id = 0;
  param.platform_config.type = ChipType::kAx650;
  param.depth_num = 1;

  void* handle = nullptr;
  int ret = ops_.Create(&handle, &param);
  ASSERT_EQ(ret, 0);

  const char* doc =
      "第一章 平台注册规范：用户须使用真实身份信息注册。\n"
      "第二章 售后退款条例：平台支持自签收之日起7天无理由退货。";
  CompanyDocInputStruct in0{301, "请问支持7天退款吗？", doc};
  CompanyDocOutputStruct out0{};

  NamedIoBatch inputs(1);
  NamedIoBatch outputs(1);
  inputs[0]["rag_channel.doc_in"] = std::shared_ptr<void>(&in0, [](void*) {});
  outputs[0]["rag_channel.doc_out"] =
      std::shared_ptr<void>(&out0, [](void*) {});

  ret = ops_.Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out0.request_id, 301);
  EXPECT_GT(out0.chunk_count, 0);
  EXPECT_TRUE(strlen(out0.answer_text) > 0);

  ops_.Destroy(handle);
}

// 15. 业务 4 (智能对话风控质检) 平台 Operator 测试
TEST_F(PlatformOperatorTest, DialogueComplianceAuditPipeline) {
  std::string conf_path = GetConfPath("configs/pipeline_dialogue_audit.conf");

  CreateParam param{};
  param.cfg_file_name = conf_path.c_str();
  param.platform_config.batch_size = 2;
  param.platform_config.device_id = 0;
  param.platform_config.type = ChipType::kAx650;
  param.depth_num = 1;

  void* handle = nullptr;
  int ret = ops_.Create(&handle, &param);
  ASSERT_EQ(ret, 0);

  CompanyAuditInputStruct in0{401, "加我微信转账，给你打八折私下结算",
                              "在线客服"};
  CompanyAuditOutputStruct out0{};

  NamedIoBatch inputs(1);
  NamedIoBatch outputs(1);
  inputs[0]["audit_module.audit_in"] =
      std::shared_ptr<void>(&in0, [](void*) {});
  outputs[0]["audit_module.audit_out"] =
      std::shared_ptr<void>(&out0, [](void*) {});

  ret = ops_.Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out0.request_id, 401);
  EXPECT_STREQ(out0.risk_level, "HIGH_RISK");

  ops_.Destroy(handle);
}

// 16. 业务 5 (多模态 OCR 图文票据) 平台 Operator 测试 ("frame" / "od_out")
TEST_F(PlatformOperatorTest, OcrDocQaPipeline) {
  std::string conf_path = GetConfPath("configs/pipeline_ocr_doc_qa.conf");

  CreateParam param{};
  param.cfg_file_name = conf_path.c_str();
  param.platform_config.batch_size = 2;
  param.platform_config.device_id = 0;
  param.platform_config.type = ChipType::kAx650;
  param.depth_num = 1;

  void* handle = nullptr;
  int ret = ops_.Create(&handle, &param);
  ASSERT_EQ(ret, 0);

  CompanyOcrDocInputStruct in0{501, "/tmp/invoice_test.jpg",
                               "提取发票金额与日期"};
  CompanyOcrDocOutputStruct out0{};

  NamedIoBatch inputs(1);
  NamedIoBatch outputs(1);
  inputs[0]["camera_0.frame"] = std::shared_ptr<void>(&in0, [](void*) {});
  outputs[0]["camera_0.od_out"] = std::shared_ptr<void>(&out0, [](void*) {});

  ret = ops_.Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out0.request_id, 501);
  EXPECT_GT(out0.detected_box_count, 0);

  ops_.Destroy(handle);
}

// 17. 业务 6 (语音识别与意图) 平台 Operator 测试
TEST_F(PlatformOperatorTest, AudioAsrIntentPipeline) {
  std::string conf_path = GetConfPath("configs/pipeline_audio_asr_intent.conf");

  CreateParam param{};
  param.cfg_file_name = conf_path.c_str();
  param.platform_config.batch_size = 2;
  param.platform_config.device_id = 0;
  param.platform_config.type = ChipType::kAx650;
  param.depth_num = 1;

  void* handle = nullptr;
  int ret = ops_.Create(&handle, &param);
  ASSERT_EQ(ret, 0);

  std::vector<float> pcm(1600, 0.1f);
  CompanyAudioInputStruct in0{601, pcm.data(), static_cast<int>(pcm.size()),
                              16000};
  CompanyAudioOutputStruct out0{};

  NamedIoBatch inputs(1);
  NamedIoBatch outputs(1);
  inputs[0]["mic_0.audio_in"] = std::shared_ptr<void>(&in0, [](void*) {});
  outputs[0]["mic_0.audio_out"] = std::shared_ptr<void>(&out0, [](void*) {});

  ret = ops_.Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out0.request_id, 601);
  EXPECT_TRUE(strlen(out0.transcribed_text) > 0);

  ops_.Destroy(handle);
}

// 18. 业务 7 (纯语义精排打分) 平台 Operator 测试
TEST_F(PlatformOperatorTest, CrossRerankPipeline) {
  std::string conf_path = GetConfPath("configs/pipeline_cross_rerank.conf");

  CreateParam param{};
  param.cfg_file_name = conf_path.c_str();
  param.platform_config.batch_size = 2;
  param.platform_config.device_id = 0;
  param.platform_config.type = ChipType::kAx650;
  param.depth_num = 1;

  void* handle = nullptr;
  int ret = ops_.Create(&handle, &param);
  ASSERT_EQ(ret, 0);

  const char* candidates[2] = {"退货政策是七天无理由退换", "今日北京晴天"};
  CompanyRerankBatchInputStruct in0{
      701, "怎么退换货？", {candidates[0], candidates[1]}, 2};
  CompanyRerankBatchOutputStruct out0{};

  NamedIoBatch inputs(1);
  NamedIoBatch outputs(1);
  inputs[0]["ranker.rerank_in"] = std::shared_ptr<void>(&in0, [](void*) {});
  outputs[0]["ranker.rerank_out"] = std::shared_ptr<void>(&out0, [](void*) {});

  ret = ops_.Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out0.request_id, 701);
  EXPECT_EQ(out0.count, 2);

  ops_.Destroy(handle);
}

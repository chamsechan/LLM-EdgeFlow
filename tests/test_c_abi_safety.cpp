#include <gtest/gtest.h>

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "adapter/biz_adapter_registry.h"
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

class CAbiSafetyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    alg_framework::BusinessAdapterRegistry::Instance()
        .ResetConflictForTesting();
    Alg_Init();
  }
  void TearDown() override {
    Alg_DeInit();
    alg_framework::BusinessAdapterRegistry::Instance()
        .ResetConflictForTesting();
  }
};

// 1. 测试空指针与异常安全防御机制 (noexcept barrier)
TEST_F(CAbiSafetyTest, NullPointerSafety) {
  EXPECT_NE(Alg_Create(nullptr, nullptr), 0);

  CompanyAlgParamCreate param;
  param.config_file_path = "";
  void* handle = nullptr;
  EXPECT_NE(Alg_Create(&handle, &param), 0);

  std::vector<void*> inputs;
  std::vector<void*> outputs;
  EXPECT_NE(Alg_Process(nullptr, inputs, outputs), 0);
  EXPECT_NE(Alg_Control(nullptr, nullptr), 0);
  EXPECT_NE(Alg_Destroy(nullptr), 0);
}

// 2. 测试句柄快速创建与销毁循环 (50轮生命周期与资源泄露检测)
TEST_F(CAbiSafetyTest, HandleLifecycleStressCycles50) {
  std::string cfg = GetConfigPath("configs/pipeline_keyword_match.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

  for (int cycle = 0; cycle < 50; ++cycle) {
    void* handle = nullptr;
    int ret = Alg_Create(&handle, &param);
    ASSERT_EQ(ret, 0) << "Failed to create handle at cycle " << cycle;
    ASSERT_NE(handle, nullptr);

    ret = Alg_Destroy(handle);
    EXPECT_EQ(ret, 0) << "Failed to destroy handle at cycle " << cycle;
  }
}

// 3. 测试通过 C ABI 接口全流程调用与动态控制规则生效
TEST_F(CAbiSafetyTest, EndToEndDynamicControlAndVerification) {
  std::string cfg = GetConfigPath("configs/pipeline_keyword_match.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

  void* handle = nullptr;
  int ret = Alg_Create(&handle, &param);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(handle, nullptr);

  // 下发动态规则
  CompanyAlgParamControl ctrl;
  ctrl.control_cmd = 1;
  ctrl.json_param_str = "{\"categories\": {\"TEST_VIP\": [\"VIP\"]}}";
  ret = Alg_Control(handle, &ctrl);
  EXPECT_EQ(ret, 0);

  // 执行推理
  CompanyKeywordInputStruct req0{101, "请联系VIP专员"};
  CompanyKeywordInputStruct req1{102, "普通闲聊文本"};
  std::vector<void*> inputs = {&req0, &req1};

  CompanyKeywordOutputStruct out0;
  CompanyKeywordOutputStruct out1;
  std::vector<void*> outputs = {&out0, &out1};

  ret = Alg_Process(handle, inputs, outputs);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(out0.is_hit, 1);
  EXPECT_EQ(out1.is_hit, 0);
  EXPECT_TRUE(std::string(out0.match_result_json).find("TEST_VIP") !=
              std::string::npos);

  ret = Alg_Destroy(handle);
  EXPECT_EQ(ret, 0);
}

// 4. 测试输出缓冲区容量不足与所需容量回填契约 (ACC-003)
TEST_F(CAbiSafetyTest, OutputCapacityInsufficientAndFeedbackContract) {
  std::string cfg = GetConfigPath("configs/pipeline_keyword_match.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

  void* handle = nullptr;
  ASSERT_EQ(Alg_Create(&handle, &param), 0);

  CompanyKeywordInputStruct req0{101, "请联系VIP专员"};
  CompanyKeywordInputStruct req1{102, "普通闲聊文本"};
  const void* inputs[2] = {&req0, &req1};

  CompanyKeywordOutputStruct out0;
  void* outputs[1] = {&out0};

  // 1) 传入 outputs = nullptr 且 capacity = 0 (标准容量预查)，必须返回 -4
  // 并回填所需容量为 2
  int num_outputs = 0;
  int ret = Alg_Process(handle, inputs, 2, nullptr, &num_outputs);
  EXPECT_EQ(ret, -4);
  EXPECT_EQ(num_outputs, 2);

  // 2) 传入 outputs 有效但容量为 1 (小于需要的 2)，应该返回 -4 并回填所需容量为
  // 2
  num_outputs = 1;
  ret = Alg_Process(handle, inputs, 2, outputs, &num_outputs);
  EXPECT_EQ(ret, -4);
  EXPECT_EQ(num_outputs, 2);

  EXPECT_EQ(Alg_Destroy(handle), 0);
}

// 5. 测试输入与输出空槽位确定性拦截 (ACC-003)
TEST_F(CAbiSafetyTest, NullSlotInBatchInputsOrOutputs) {
  std::string cfg = GetConfigPath("configs/pipeline_keyword_match.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

  void* handle = nullptr;
  ASSERT_EQ(Alg_Create(&handle, &param), 0);

  CompanyKeywordInputStruct req0{101, "请联系VIP专员"};
  const void* inputs_with_null[2] = {&req0, nullptr};  // 第二个槽位为空

  CompanyKeywordOutputStruct out0, out1;
  void* outputs[2] = {&out0, &out1};
  int num_outputs = 2;

  // 输入包含空指针 -> 必须确定性返回 -3
  int ret = Alg_Process(handle, inputs_with_null, 2, outputs, &num_outputs);
  EXPECT_EQ(ret, -3);

  // 输出包含空指针 -> 必须确定性返回 -4
  const void* valid_inputs[2] = {&req0, &req0};
  void* outputs_with_null[2] = {&out0, nullptr};
  num_outputs = 2;
  ret = Alg_Process(handle, valid_inputs, 2, outputs_with_null, &num_outputs);
  EXPECT_EQ(ret, -4);

  EXPECT_EQ(Alg_Destroy(handle), 0);
}

// 6. 测试 Adapter 注册冲突防护与 Descriptor 机器可读性 (ACC-005)
TEST_F(CAbiSafetyTest, AdapterRegistryConflictDetectionAndDescriptor) {
  auto& registry = alg_framework::BusinessAdapterRegistry::Instance();
  auto doc_adapter = registry.GetAdapter(ALG_BIZ_TYPE_DOC_QA);
  ASSERT_NE(doc_adapter, nullptr);

  // 验证 Descriptor
  const auto& desc = doc_adapter->GetDescriptor();
  EXPECT_EQ(desc.biz_type, ALG_BIZ_TYPE_DOC_QA);
  EXPECT_EQ(desc.biz_name, "DocQA");
  EXPECT_EQ(desc.abi_version, "2.0.0");
  EXPECT_GT(desc.max_batch_size, 0);

  // 测试重复 BizType 注册拦截
  bool reg_dup_ret = registry.RegisterAdapter(doc_adapter);
  EXPECT_FALSE(reg_dup_ret) << "Duplicate biz_type registration must fail";
}

// 7. 测试 RuntimeOptions 与设备参数贯通 (ACC-004)
TEST_F(CAbiSafetyTest, RuntimeOptionsAndDevicePropagation) {
  std::string cfg = GetConfigPath("configs/pipeline_doc_qa.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;  // 显式指定设备 0
  param.biz_type = ALG_BIZ_TYPE_DOC_QA;

  void* handle0 = nullptr;
  ASSERT_EQ(Alg_Create(&handle0, &param), 0);
  EXPECT_EQ(Alg_Destroy(handle0), 0);

  param.device_id = 1;  // 显式指定设备 1
  void* handle1 = nullptr;
  ASSERT_EQ(Alg_Create(&handle1, &param), 0);
  EXPECT_EQ(Alg_Destroy(handle1), 0);
}

// 8. 测试 UNKNOWN 业务与未注册业务在 Alg_Create 前置拦截 (REV2-001)
TEST_F(CAbiSafetyTest, UnknownAndUnregisteredBizRejectionInCreate) {
  std::string cfg = GetConfigPath("configs/pipeline_keyword_match.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;

  // 1) 传入 ALG_BIZ_TYPE_UNKNOWN 必须被 Alg_Create 明确拒绝返回 -5
  param.biz_type = ALG_BIZ_TYPE_UNKNOWN;
  void* handle = nullptr;
  int ret = Alg_Create(&handle, &param);
  EXPECT_EQ(ret, -5);
  EXPECT_EQ(handle, nullptr);

  // 2) 传入越界/未注册业务枚举 9999 必须被 Alg_Create 明确拒绝返回 -5
  param.biz_type = static_cast<CompanyAlgBizType>(9999);
  ret = Alg_Create(&handle, &param);
  EXPECT_EQ(ret, -5);
  EXPECT_EQ(handle, nullptr);
}

// 9. 测试 Registry 冲突 fail-closed 导致 Alg_Init 失败 (REV2-003)
TEST_F(CAbiSafetyTest, FailClosedRegistryConflictAndInitFailure) {
  auto& registry = alg_framework::BusinessAdapterRegistry::Instance();
  registry.ResetConflictForTesting();

  // 初始干净状态 Alg_Init 成功
  EXPECT_EQ(Alg_Init(), 0);

  // 注册冲突（重复注册 DocQA 业务）
  auto doc_adapter = registry.GetAdapter(ALG_BIZ_TYPE_DOC_QA);
  ASSERT_NE(doc_adapter, nullptr);
  bool reg_ret = registry.RegisterAdapter(doc_adapter);
  EXPECT_FALSE(reg_ret);
  EXPECT_TRUE(registry.HasRegistrationConflict());

  // 注册冲突发生后，Alg_Init 必须 fail-closed 返回 -6
  EXPECT_EQ(Alg_Init(), -6);

  // 测试结束后清理恢复干净状态
  registry.ResetConflictForTesting();
  EXPECT_FALSE(registry.HasRegistrationConflict());
  EXPECT_EQ(Alg_Init(), 0);
}

// 10. 测试 Adapter Descriptor max_batch_size 契约强制执行 (REV2-005)
TEST_F(CAbiSafetyTest, AdapterDescriptorMaxBatchSizeEnforcement) {
  std::string cfg = GetConfigPath("configs/pipeline_keyword_match.json");
  CompanyAlgParamCreate param;
  param.config_file_path = cfg.c_str();
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

  void* handle = nullptr;
  ASSERT_EQ(Alg_Create(&handle, &param), 0);

  // 构造 65 条输入数据 (超过 max_batch_size = 64 上限)
  std::vector<CompanyKeywordInputStruct> reqs(65, {1, "测试输入"});
  std::vector<const void*> inputs(65);
  for (int i = 0; i < 65; ++i) inputs[i] = &reqs[i];

  std::vector<CompanyKeywordOutputStruct> outs(65);
  std::vector<void*> outputs(65);
  for (int i = 0; i < 65; ++i) outputs[i] = &outs[i];
  int num_outputs = 65;

  // 超过 max_batch_size -> 必须被 ValidateBatchPreFlight 前置拦截返回 -3
  int ret =
      Alg_Process(handle, inputs.data(), 65, outputs.data(), &num_outputs);
  EXPECT_EQ(ret, -3);

  EXPECT_EQ(Alg_Destroy(handle), 0);
}

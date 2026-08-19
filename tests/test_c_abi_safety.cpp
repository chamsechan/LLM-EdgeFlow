#include <gtest/gtest.h>

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "adapter/business_adapter_registry.h"
#include "company_alg_interface.h"

std::string GetConfigPath(const std::string& rel_path) {
  FILE* fp = fopen(rel_path.c_str(), "r");
  if (fp) {
    fclose(fp);
    return rel_path;
  }
  return "../" + rel_path;
}

class CAbiSafetyTest : public ::testing::Test {
 protected:
  void SetUp() override { Alg_Init(); }
  void TearDown() override { Alg_DeInit(); }
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

  // 1) 传入容量为 0，应该返回 -4 并回填所需容量为 2
  int num_outputs = 0;
  int ret = Alg_Process(handle, inputs, 2, outputs, &num_outputs);
  EXPECT_EQ(ret, -4);
  EXPECT_EQ(num_outputs, 2);

  // 2) 传入容量为 1 (小于需要的 2)，应该返回 -4 并回填所需容量为 2
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

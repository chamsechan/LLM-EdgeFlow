#include <gtest/gtest.h>

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

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

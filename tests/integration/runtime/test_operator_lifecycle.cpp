#include <gtest/gtest.h>

#include <string>

#include "platform_mock/operator_data_types.h"
#include "tests/support/operator_test_fixture.h"

namespace llm_edgeflow {
using test_support::ScopedTestOperator;

using OperatorLifecycleTest = test_support::OperatorTestFixture;

TEST_F(OperatorLifecycleTest, HandlesCloseIndependently) {
  ScopedTestOperator first(ops_);
  ScopedTestOperator second(ops_);
  ASSERT_EQ(first.Create("configs/pipeline_keyword_match_rules.conf"), 0)
      << first.create_diagnostic();
  ASSERT_EQ(second.Create("configs/pipeline_keyword_match_rules.conf"), 0)
      << second.create_diagnostic();
  EXPECT_EQ(first.Close(), 0) << first.close_diagnostic();
  operator_api::ControlUpdateRulesParam rules{
      "{\"categories\":{\"VIP_SERVICE\":[\"VIP\"]}}"};
  EXPECT_EQ(
      ops_.Control(second.get(),
                   static_cast<int>(operator_api::ControlCommand::kUpdateRules),
                   &rules),
      0)
      << operator_api::GetOperatorLastError();
  EXPECT_EQ(second.Close(), 0) << second.close_diagnostic();
}

TEST(OperatorGlobalLifecycleTest, RapidGlobalLifecycleInitDeInit) {
  using namespace llm_edgeflow;
  auto op = operator_api::Get_LLM_EDGEFLOW_OperatorTable();

  for (int cycle = 0; cycle < 30; ++cycle) {
    EXPECT_EQ(op.Init(), 0);

    operator_api::CreateParam param{};
    param.model_path = ".";
    param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
    param.device_id = 0;
    param.compute_platform = operator_api::ComputePlatform::kCpu;
    param.max_frame_depth = 25;

    void* handle = nullptr;
    ASSERT_EQ(op.Create(&handle, &param), 0);
    ASSERT_NE(handle, nullptr);

    std::string sentence = "循环生命周期压测文本";
    CompanyString cs_sentence{static_cast<int32_t>(sentence.size()),
                              const_cast<char*>(sentence.data())};
    CompanyOperatorKeywordInput in_req{};
    in_req.service_type = kMockServiceKeywordMatch;
    in_req.sentence_text = &cs_sentence;

    operator_api::NamedIoBatch inputs(1);
    inputs[0]["client_channel.keyword_in"] =
        operator_api::MakeBorrowedOperatorInput(&in_req);
    operator_api::NamedIoBatch outputs(1);
    outputs[0]["client_channel.json_out"] = std::shared_ptr<void>();

    int ret = op.Process(handle, inputs, outputs);
    EXPECT_EQ(ret, 0);

    outputs.clear();
    EXPECT_EQ(op.Destroy(handle), 0);
    EXPECT_EQ(op.DeInit(), 0);
  }
}

}  // namespace llm_edgeflow

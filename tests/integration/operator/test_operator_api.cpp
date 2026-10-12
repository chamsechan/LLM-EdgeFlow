#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <future>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "adapter/operator/mock/operator_error_mapping.h"
#include "edgeflow/operator/interface.h"

namespace llm_edgeflow {
namespace {
namespace api = operator_api;

class OperatorEntryTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_EQ(ops_.Init(), 0); }
  void TearDown() override {
    outputs_.clear();
    if (handle_) {
      EXPECT_EQ(ops_.Destroy(handle_), 0);
    }
    EXPECT_EQ(ops_.DeInit(), 0);
  }
  api::CreateParam ValidCreate() {
    api::CreateParam params;
    params.model_path = ".";
    params.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
    params.compute_platform = api::ComputePlatform::kCpu;
    params.max_frame_depth = 2;
    return params;
  }
  void Input() {
    text_ = "初始化";
    string_ = {static_cast<int32_t>(text_.size()), text_.data()};
    request_ = {kMockServiceKeywordMatch, &string_};
    inputs_ = {
        {{"source.keyword_in", api::MakeBorrowedOperatorInput(&request_)}}};
    outputs_ = {{{"response.json_out", nullptr}}};
  }
  api::OperatorFunc ops_ = api::Get_LLM_EDGEFLOW_OperatorTable();
  void* handle_ = nullptr;
  std::string text_;
  CompanyString string_{};
  CompanyOperatorKeywordInput request_{};
  api::NamedIoBatch inputs_, outputs_;
};

TEST_F(OperatorEntryTest, CreateRequiresAnEmptyHandleAndRejectsInvalidOptions) {
  auto params = ValidCreate();
  EXPECT_EQ(ops_.Create(nullptr, &params), -1);
  EXPECT_EQ(ops_.Create(&handle_, nullptr), -1);
  EXPECT_EQ(handle_, nullptr);
  void* occupied = reinterpret_cast<void*>(uintptr_t{1});
  EXPECT_EQ(ops_.Create(&occupied, &params), -1);
  EXPECT_EQ(occupied, reinterpret_cast<void*>(uintptr_t{1}));
  enum class Defect { kRoot, kConfig, kDevice, kPlatform, kDepth, kMissing };
  for (Defect defect : {Defect::kRoot, Defect::kConfig, Defect::kDevice,
                        Defect::kPlatform, Defect::kDepth, Defect::kMissing}) {
    SCOPED_TRACE(static_cast<int>(defect));
    params = ValidCreate();
    switch (defect) {
      case Defect::kRoot:
        params.model_path = "";
        break;
      case Defect::kConfig:
        params.cfg_file_name = nullptr;
        break;
      case Defect::kDevice:
        params.device_id = -1;
        break;
      case Defect::kPlatform:
        params.compute_platform = static_cast<api::ComputePlatform>(99);
        break;
      case Defect::kDepth:
        params.max_frame_depth = 1025;
        break;
      case Defect::kMissing:
        params.cfg_file_name = "missing-operator-entry.conf";
        break;
    }
    EXPECT_EQ(ops_.Create(&handle_, &params), -2);
    EXPECT_EQ(handle_, nullptr);
    EXPECT_NE(std::string(api::GetOperatorLastError()), "");
  }
  params = ValidCreate();
  ASSERT_EQ(ops_.Create(&handle_, &params), 0) << api::GetOperatorLastError();
}

TEST_F(OperatorEntryTest, BatchShapeAndOccupiedOutputsFailBeforePublishing) {
  auto params = ValidCreate();
  ASSERT_EQ(ops_.Create(&handle_, &params), 0);
  Input();
  api::NamedIoBatch empty;
  EXPECT_EQ(ops_.Process(handle_, empty, outputs_), -3);
  EXPECT_EQ(ops_.Process(handle_, inputs_, empty), -4);
  auto extra_inputs = inputs_;
  extra_inputs.push_back(inputs_[0]);
  EXPECT_EQ(ops_.Process(handle_, extra_inputs, outputs_), -3);
  auto large_inputs = inputs_;
  large_inputs.resize(3, inputs_[0]);
  api::NamedIoBatch large_outputs(3, outputs_[0]);
  EXPECT_EQ(ops_.Process(handle_, large_inputs, large_outputs), -3);
  auto occupied = std::make_shared<int>(8);
  outputs_[0]["response.json_out"] = occupied;
  EXPECT_EQ(ops_.Process(handle_, inputs_, outputs_), -4);
  EXPECT_EQ(outputs_[0].at("response.json_out"), occupied);
  outputs_[0]["response.json_out"].reset();
  ASSERT_EQ(ops_.Process(handle_, inputs_, outputs_), 0);
  EXPECT_NE(outputs_[0].at("response.json_out"), nullptr);
}

TEST_F(OperatorEntryTest, ControlMapsRequestAndUnsupportedCommandStages) {
  auto params = ValidCreate();
  ASSERT_EQ(ops_.Create(&handle_, &params), 0);
  api::ControlJsonParam malformed{1000003, "[]"};
  EXPECT_EQ(ops_.Control(handle_, static_cast<int>(api::ControlCommand::kJson),
                         &malformed),
            -2);
  api::ControlJsonParam unknown{2000000099, "{}"};
  EXPECT_EQ(ops_.Control(handle_, static_cast<int>(api::ControlCommand::kJson),
                         &unknown),
            -7);
  EXPECT_NE(std::string(api::GetOperatorLastError()).find("internal code"),
            std::string::npos);
  api::ControlUpdateRulesParam valid{
      R"({"categories":{"CHANGED":["初始化"]}})"};
  ASSERT_EQ(
      ops_.Control(handle_, static_cast<int>(api::ControlCommand::kUpdateRules),
                   &valid),
      0);
  Input();
  ASSERT_EQ(ops_.Process(handle_, inputs_, outputs_), 0);
}

TEST_F(OperatorEntryTest, DestroyWithOutstandingLeaseConsumesTheHandle) {
  auto params = ValidCreate();
  ASSERT_EQ(ops_.Create(&handle_, &params), 0);
  Input();
  ASSERT_EQ(ops_.Process(handle_, inputs_, outputs_), 0);
  void* consumed = std::exchange(handle_, nullptr);
  EXPECT_EQ(ops_.Destroy(consumed), -1);
  EXPECT_NE(std::string(api::GetOperatorLastError()).find("unreturned"),
            std::string::npos);
  EXPECT_EQ(ops_.Destroy(consumed), -1);
  EXPECT_EQ(ops_.Process(consumed, inputs_, outputs_), -1);
  api::ControlJsonParam control{1000003, "{}"};
  EXPECT_EQ(ops_.Control(consumed, static_cast<int>(api::ControlCommand::kJson),
                         &control),
            -1);
  // A stale weak-pool deleter may be released, but its payload is already
  // freed.
  outputs_.clear();
}

TEST_F(OperatorEntryTest,
       DeInitConsumesAllActiveHandlesAndLaterInitCanCreateAgain) {
  auto params = ValidCreate();
  ASSERT_EQ(ops_.Create(&handle_, &params), 0);
  void* second = nullptr;
  ASSERT_EQ(ops_.Create(&second, &params), 0);
  void* first = std::exchange(handle_, nullptr);
  EXPECT_EQ(ops_.DeInit(), 0);
  EXPECT_EQ(ops_.Destroy(first), -1);
  EXPECT_EQ(ops_.Destroy(second), -1);
  ASSERT_EQ(ops_.Init(), 0);
  ASSERT_EQ(ops_.Create(&handle_, &params), 0);
}

TEST_F(OperatorEntryTest, DiagnosticsBelongToTheCallingThread) {
  EXPECT_EQ(ops_.Process(nullptr, inputs_, outputs_), -1);
  const std::string main_error = api::GetOperatorLastError();
  auto other = std::async(std::launch::async, [ops = ops_] {
    const std::string before = api::GetOperatorLastError();
    void* handle = nullptr;
    const int code = ops.Create(&handle, nullptr);
    return std::make_tuple(before, code,
                           std::string(api::GetOperatorLastError()));
  });
  const auto [before, code, after] = other.get();
  EXPECT_TRUE(before.empty());
  EXPECT_EQ(code, -1);
  EXPECT_NE(after.find("CreateParam"), std::string::npos);
  EXPECT_NE(main_error, after);
  EXPECT_EQ(std::string(api::GetOperatorLastError()), main_error);
}

TEST(OperatorFailureMappingTest, PublicCategoriesRetainStageAndInternalCause) {
  struct Case {
    OperatorFailureStage stage;
    int code, expected;
    const char* stage_name;
  };
  const std::vector<Case> cases{
      {OperatorFailureStage::kCreatePreparation, -35, -2, "Create preparation"},
      {OperatorFailureStage::kCreatePreparation, -5, -5, "Create preparation"},
      {OperatorFailureStage::kCreatePreparation, -6, -6, "Create preparation"},
      {OperatorFailureStage::kCreatePreparation, -99, -99,
       "Create preparation"},
      {OperatorFailureStage::kCreatePreparation, -100, -100,
       "Create preparation"},
      {OperatorFailureStage::kProcessExecution, -35, -100,
       "Pipeline execution"},
      {OperatorFailureStage::kControlRequest, -35, -2, "Control request"},
      {OperatorFailureStage::kControlUnsupported, -35, -7, "Control command"},
      {OperatorFailureStage::kControlExecution, -35, -100,
       "Control node update"},
      {OperatorFailureStage::kControlRuntime, -99, -99, "Control runtime"}};
  for (const auto& row : cases) {
    SCOPED_TRACE(row.stage_name);
    EXPECT_EQ(PublicFailureCode(row.stage, row.code), row.expected);
    const auto message =
        DescribeOperatorFailure(row.stage, row.code, "literal cause");
    EXPECT_NE(message.find(row.stage_name), std::string::npos);
    EXPECT_NE(message.find("internal code " + std::to_string(row.code)),
              std::string::npos);
    EXPECT_NE(message.find("literal cause"), std::string::npos);
  }
}

}  // namespace
}  // namespace llm_edgeflow

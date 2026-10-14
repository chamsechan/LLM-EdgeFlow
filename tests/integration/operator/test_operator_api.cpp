#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <nlohmann/json.hpp>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "adapter/operator/mock/operator_error_mapping.h"
#include "contracts/error_codes.h"
#include "edgeflow/log.h"
#include "edgeflow/operator/interface.h"

namespace llm_edgeflow {
namespace {
namespace api = operator_api;

class OperatorEntryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_log_level_ = AlgBase_getLogLevelByName(nullptr);
    ASSERT_EQ(ops_.Init(), 0);
  }
  void TearDown() override {
    outputs_.clear();
    if (handle_) {
      EXPECT_EQ(ops_.Destroy(handle_), 0);
    }
    EXPECT_EQ(ops_.DeInit(), 0);
    AlgBase_setLogLevelByName(nullptr, saved_log_level_);
    if (!directory_.empty()) {
      std::error_code error;
      std::filesystem::remove_all(directory_, error);
      EXPECT_FALSE(error) << error.message();
    }
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
  int saved_log_level_ = 0;
  std::filesystem::path directory_;
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
  for (const int command : {3, 99}) {
    EXPECT_EQ(ops_.Control(handle_, command, &unknown), -2);
    EXPECT_NE(std::string(api::GetOperatorLastError())
                  .find("Unsupported or unknown ControlCommand"),
              std::string::npos);
  }
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

TEST_F(OperatorEntryTest,
       InputRejectionLogsOnceAndFilteringPreservesDiagnostic) {
  auto params = ValidCreate();
  ASSERT_EQ(ops_.Create(&handle_, &params), 0) << api::GetOperatorLastError();
  Input();
  api::NamedIoBatch empty;
  ASSERT_EQ(AlgBase_setLogLevelByName(nullptr, E_ALG_BASE_LOG_LEVEL_WARNING),
            0);
  testing::internal::CaptureStderr();
  const int result = ops_.Process(handle_, empty, outputs_);
  const auto record = testing::internal::GetCapturedStderr();
  EXPECT_EQ(result, -3);
  EXPECT_NE(record.find("[Warning]"), std::string::npos);
  EXPECT_EQ(record.find("[Error]"), std::string::npos);
  EXPECT_EQ(std::count(record.begin(), record.end(), '\n'), 1);
  EXPECT_NE(record.find("operation=Operator_Process"), std::string::npos);
  EXPECT_NE(record.find("code=INVALID_INPUT(" +
                        std::to_string(error::kInvalidInput) + ")"),
            std::string::npos);
  EXPECT_NE(record.find("platform_code=-3"), std::string::npos);
  EXPECT_NE(record.find("stage=Runtime"), std::string::npos);
  EXPECT_EQ(record.find("Empty inputs NamedIoBatch"), std::string::npos);
  const std::string diagnostic = api::GetOperatorLastError();
  EXPECT_NE(diagnostic.find("INVALID_INPUT"), std::string::npos);
  EXPECT_NE(diagnostic.find("Empty inputs NamedIoBatch"), std::string::npos);
  EXPECT_EQ(outputs_[0].at("response.json_out"), nullptr);

  ASSERT_EQ(AlgBase_setLogLevelByName(nullptr, E_ALG_BASE_LOG_LEVEL_FATAL), 0);
  testing::internal::CaptureStderr();
  const int filtered_result = ops_.Process(handle_, empty, outputs_);
  const auto filtered_record = testing::internal::GetCapturedStderr();
  EXPECT_EQ(filtered_result, result);
  EXPECT_TRUE(filtered_record.empty());
  EXPECT_EQ(std::string(api::GetOperatorLastError()), diagnostic);
}

TEST_F(OperatorEntryTest,
       PipelineFailureLogsOneActionableRecordAtOperatorBoundary) {
  char directory[] = "/tmp/edgeflow-operator-log-XXXXXX";
  ASSERT_NE(mkdtemp(directory), nullptr);
  directory_ = directory;
  std::ifstream source("configs/pipeline_keyword_match_rules.json");
  ASSERT_TRUE(source.good());
  auto pipeline = nlohmann::json::parse(source);
  pipeline["pipeline"][0]["inputs"]["text"] = "bounded.text";
  pipeline["pipeline"].push_back(
      {{"type", "text_template"},
       {"name", "bounded"},
       {"inputs", {{"primary", "input.sentence_text"}}},
       {"params", {{"template", "{{primary}}"}, {"max_length", 1}}}});
  {
    std::ofstream config(directory_ / "pipeline.json");
    config << pipeline.dump();
    ASSERT_TRUE(config.good());
  }
  {
    std::ofstream config(directory_ / "deployment.conf");
    config << R"({"pipe_path":"pipeline.json"})";
    ASSERT_TRUE(config.good());
  }
  auto params = ValidCreate();
  params.model_path = directory;
  params.cfg_file_name = "deployment.conf";
  ASSERT_EQ(ops_.Create(&handle_, &params), 0) << api::GetOperatorLastError();
  Input();
  ASSERT_EQ(AlgBase_setLogLevelByName(nullptr, E_ALG_BASE_LOG_LEVEL_VERBOSE),
            0);
  testing::internal::CaptureStderr();
  const int result = ops_.Process(handle_, inputs_, outputs_);
  const auto record = testing::internal::GetCapturedStderr();
  EXPECT_EQ(result, -100);
  const auto error_record = record.find("[Error]");
  ASSERT_NE(error_record, std::string::npos);
  EXPECT_EQ(record.find("[Error]", error_record + 1), std::string::npos);
  EXPECT_EQ(record.find("[Warning]"), std::string::npos);
  EXPECT_NE(record.find("operation=Operator_Process"), std::string::npos);
  EXPECT_NE(record.find("code=RENDERED_OUTPUT_TOO_LONG(" +
                        std::to_string(error::kRenderedOutputTooLong) + ")"),
            std::string::npos);
  EXPECT_NE(record.find("platform_code=-100"), std::string::npos);
  EXPECT_NE(record.find("stage=Pipeline execution"), std::string::npos);
  EXPECT_EQ(record.find("Rendered prompt exceeds max_length of 1"),
            std::string::npos);
  const std::string diagnostic = api::GetOperatorLastError();
  EXPECT_NE(diagnostic.find("RENDERED_OUTPUT_TOO_LONG"), std::string::npos);
  EXPECT_NE(diagnostic.find("internal code " +
                            std::to_string(error::kRenderedOutputTooLong)),
            std::string::npos);
  EXPECT_NE(diagnostic.find("Rendered prompt exceeds max_length of 1"),
            std::string::npos);
  EXPECT_EQ(outputs_[0].at("response.json_out"), nullptr);

  ASSERT_EQ(AlgBase_setLogLevelByName(nullptr, E_ALG_BASE_LOG_LEVEL_FATAL), 0);
  testing::internal::CaptureStderr();
  const int filtered_result = ops_.Process(handle_, inputs_, outputs_);
  const auto filtered_record = testing::internal::GetCapturedStderr();
  EXPECT_EQ(filtered_result, result);
  EXPECT_TRUE(filtered_record.empty());
  EXPECT_EQ(std::string(api::GetOperatorLastError()), diagnostic);
}

TEST_F(OperatorEntryTest, BoundaryLogsOmitRequestContentButDiagnosticKeepsIt) {
  auto params = ValidCreate();
  ASSERT_EQ(ops_.Create(&handle_, &params), 0) << api::GetOperatorLastError();
  // 未闭合的 JSON 字符串：解析异常的 what() 会携带 "last read" 原文片段。
  const std::string marker = "EF_SECRET_MARKER_7f3a";
  const std::string payload =
      "{\"categories\":{\"" + marker + "\":[\"" + marker;
  api::ControlUpdateRulesParam malformed{payload.c_str()};
  ASSERT_EQ(AlgBase_setLogLevelByName(nullptr, E_ALG_BASE_LOG_LEVEL_VERBOSE),
            0);
  testing::internal::CaptureStderr();
  const int result = ops_.Control(
      handle_, static_cast<int>(api::ControlCommand::kUpdateRules), &malformed);
  const auto record = testing::internal::GetCapturedStderr();
  EXPECT_NE(result, 0);
  EXPECT_EQ(result, -2);
  EXPECT_NE(record.find("operation=Operator_Control"), std::string::npos);
  EXPECT_NE(record.find("platform_code=" + std::to_string(result)),
            std::string::npos);
  EXPECT_EQ(record.find(marker), std::string::npos) << record;
  const std::string diagnostic = api::GetOperatorLastError();
  EXPECT_NE(diagnostic.find("Invalid control JSON"), std::string::npos);
  EXPECT_NE(diagnostic.find(marker), std::string::npos);
}

TEST(OperatorFailureMappingTest, PublicCategoriesRetainStageAndInternalCause) {
  struct Case {
    OperatorFailureStage stage;
    int code, expected;
    const char* stage_name;
  };
  const std::vector<Case> cases{
      {OperatorFailureStage::kCreatePreparation, error::kValidationFailed, -2,
       "Create preparation"},
      {OperatorFailureStage::kCreatePreparation, error::kUnsupported, -5,
       "Create preparation"},
      {OperatorFailureStage::kCreatePreparation, error::kRegistryConflict, -6,
       "Create preparation"},
      {OperatorFailureStage::kCreatePreparation, error::kInternalException, -99,
       "Create preparation"},
      {OperatorFailureStage::kCreatePreparation, error::kUnknownException, -100,
       "Create preparation"},
      {OperatorFailureStage::kControlRequest, error::kInvalidArgument, -2,
       "Control request"},
      {OperatorFailureStage::kControlUnsupported, error::kUnsupportedControl,
       -7, "Control command"},
      {OperatorFailureStage::kRuntime, error::kInvalidState, -1, "Runtime"},
      {OperatorFailureStage::kRuntime, error::kInvalidArgument, -2, "Runtime"},
      {OperatorFailureStage::kRuntime, error::kInvalidInput, -3, "Runtime"},
      {OperatorFailureStage::kRuntime, error::kInvalidOutput, -4, "Runtime"},
      {OperatorFailureStage::kRuntime, error::kAllocationFailed, -4, "Runtime"},
      {OperatorFailureStage::kRuntime, error::kPoolClosed, -4, "Runtime"},
      {OperatorFailureStage::kRuntime, error::kPoolCorrupt, -4, "Runtime"},
      {OperatorFailureStage::kRuntime, error::kPoolMissing, -4, "Runtime"},
      {OperatorFailureStage::kRuntime, error::kUnsupported, -5, "Runtime"},
      {OperatorFailureStage::kRuntime, error::kRegistryConflict, -6, "Runtime"},
      {OperatorFailureStage::kRuntime, error::kUnsupportedControl, -7,
       "Runtime"},
      {OperatorFailureStage::kRuntime, error::kInternalException, -99,
       "Runtime"},
      {OperatorFailureStage::kRuntime, error::kUnknownException, -100,
       "Runtime"},
      {OperatorFailureStage::kRuntime, -35, -100, "Runtime"}};
  for (const auto& row : cases) {
    SCOPED_TRACE(row.stage_name);
    EXPECT_EQ(PublicFailureCode(row.stage, row.code), row.expected);
    const auto message =
        DescribeOperatorFailure(row.stage, row.code, "literal cause");
    EXPECT_NE(message.find(row.stage_name), std::string::npos);
    EXPECT_NE(message.find("internal code " + std::to_string(row.code)),
              std::string::npos);
    EXPECT_NE(message.find(ErrorCodeName(row.code)), std::string::npos);
    EXPECT_NE(message.find("literal cause"), std::string::npos);
  }
}

TEST(OperatorFailureMappingTest,
     SuccessAndExecutionFailuresNeverLeakInternalCodes) {
  for (const auto stage : {OperatorFailureStage::kCreatePreparation,
                           OperatorFailureStage::kProcessExecution,
                           OperatorFailureStage::kControlRequest,
                           OperatorFailureStage::kControlUnsupported,
                           OperatorFailureStage::kControlExecution,
                           OperatorFailureStage::kRuntime}) {
    EXPECT_EQ(PublicFailureCode(stage, error::kOk), 0);
  }
  for (const auto stage : {OperatorFailureStage::kProcessExecution,
                           OperatorFailureStage::kControlExecution}) {
    for (int code : {error::kInvalidInput, error::kInternalException,
                     error::kUnknownException, -35, -99}) {
      SCOPED_TRACE(code);
      EXPECT_EQ(PublicFailureCode(stage, code), -100);
    }
  }
  // A raw value that happens to equal a platform code is still unknown
  // internally.
  EXPECT_EQ(PublicFailureCode(OperatorFailureStage::kRuntime, -5), -100);
  EXPECT_EQ(PublicFailureCode(OperatorFailureStage::kCreatePreparation, -5),
            -2);
}

}  // namespace
}  // namespace llm_edgeflow

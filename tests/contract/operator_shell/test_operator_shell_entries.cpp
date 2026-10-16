#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <nlohmann/json.hpp>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "contracts/error_codes.h"
#include "edgeflow/log.h"
#include "edgeflow/operator/interface.h"
#include "tests/contract/operator_shell/operator_shell_checks.h"

namespace llm_edgeflow::operator_shell_test {
namespace {
namespace api = operator_api;

class OperatorShellTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_log_level_ = AlgBase_getLogLevelByName(nullptr);
    ASSERT_TRUE(ReturnsCategory(driver_->Init(), Category::kSuccess));
  }
  void TearDown() override {
    driver_->ReleaseOutputs();
    if (handle_) {
      EXPECT_TRUE(
          ReturnsCategory(driver_->Destroy(handle_), Category::kSuccess));
    }
    EXPECT_TRUE(ReturnsCategory(driver_->DeInit(), Category::kSuccess));
    AlgBase_setLogLevelByName(nullptr, saved_log_level_);
    if (!directory_.empty()) {
      std::error_code error;
      std::filesystem::remove_all(directory_, error);
      EXPECT_FALSE(error) << error.message();
    }
  }
  void CreateHandle(const CreateRequest& request = {}) {
    ASSERT_TRUE(ReturnsCategory(driver_->Create(&handle_, request),
                                Category::kSuccess));
    ASSERT_NE(handle_, nullptr);
  }
  std::unique_ptr<OperatorShellDriver> driver_ = MakeOperatorShellDriver();
  void* handle_ = nullptr;
  int saved_log_level_ = 0;
  std::filesystem::path directory_;
};

TEST_F(OperatorShellTest, CreateRequiresAnEmptyHandleAndRejectsInvalidOptions) {
  EXPECT_TRUE(
      ReturnsCategory(driver_->Create(nullptr, {}), Category::kInvalidHandle));
  CreateRequest null_param;
  null_param.defect = CreateDefect::kNullParam;
  EXPECT_TRUE(ReturnsCategory(driver_->Create(&handle_, null_param),
                              Category::kInvalidHandle));
  EXPECT_EQ(handle_, nullptr);
  void* occupied = reinterpret_cast<void*>(uintptr_t{1});
  EXPECT_TRUE(ReturnsCategory(driver_->Create(&occupied, {}),
                              Category::kInvalidHandle));
  EXPECT_EQ(occupied, reinterpret_cast<void*>(uintptr_t{1}));
  std::vector<CreateRequest> invalid;
  for (const auto defect :
       {CreateDefect::kEmptyModelPath, CreateDefect::kNullConfig,
        CreateDefect::kNegativeDevice, CreateDefect::kUnknownPlatform}) {
    invalid.emplace_back();
    invalid.back().defect = defect;
  }
  invalid.emplace_back();
  invalid.back().max_frame_depth = 1025;
  invalid.emplace_back();
  invalid.back().config = "missing-operator-entry.conf";
  for (size_t i = 0; i < invalid.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_TRUE(ReturnsCategory(driver_->Create(&handle_, invalid[i]),
                                Category::kInvalidParam));
    EXPECT_EQ(handle_, nullptr);
    EXPECT_NE(std::string(api::GetOperatorLastError()), "");
  }
  CreateHandle();
}

TEST_F(OperatorShellTest, BatchShapeAndOccupiedOutputsFailBeforePublishing) {
  CreateHandle();
  EXPECT_TRUE(RejectedBatchesPublishNothing(*driver_, handle_));
  EXPECT_TRUE(ReturnsCategory(driver_->Process(handle_, {{"初始化"}, 0}),
                              Category::kOutputCapacity));
  EXPECT_TRUE(ReturnsCategory(
      driver_->Process(handle_, {{"初始化", "初始化", "初始化"}, 3}),
      Category::kInvalidInput));
  EXPECT_EQ(driver_->PublishedOutputs(), 0u);
  ProcessRequest occupied{{"初始化"}, 1};
  occupied.occupy_first_output = true;
  EXPECT_TRUE(ReturnsCategory(driver_->Process(handle_, occupied),
                              Category::kOutputCapacity));
  EXPECT_TRUE(driver_->OccupiedOutputPreserved());
  EXPECT_EQ(driver_->PublishedOutputs(), 0u);
  ASSERT_TRUE(ReturnsCategory(driver_->Process(handle_, {{"初始化"}, 1}),
                              Category::kSuccess));
  EXPECT_EQ(driver_->PublishedOutputs(), 1u);
}

TEST_F(OperatorShellTest, RowsPairInOrderAndRejectedInputPublishesNothing) {
  CreateHandle();
  ASSERT_TRUE(ReturnsCategory(
      driver_->Process(handle_, {{"初始化完成", "ordinary text"}, 2}),
      Category::kSuccess));
  ASSERT_EQ(driver_->PublishedOutputs(), 2u);
  const auto first = nlohmann::json::parse(driver_->OutputJson(0));
  const auto second = nlohmann::json::parse(driver_->OutputJson(1));
  EXPECT_EQ(first.at("intent"), "SYSTEM_INIT");
  EXPECT_EQ(second.at("is_hit"), 0);
  driver_->ReleaseOutputs();
  ProcessRequest invalid{{"初始化", "初始化"}, 2};
  invalid.invalid_last_input = true;
  EXPECT_TRUE(ReturnsCategory(driver_->Process(handle_, invalid),
                              Category::kInvalidInput));
  EXPECT_EQ(driver_->PublishedOutputs(), 0u);
  ASSERT_TRUE(ReturnsCategory(driver_->Process(handle_, {{"初始化"}, 1}),
                              Category::kSuccess));
}

TEST_F(OperatorShellTest,
       BatchLimitFollowsDepthCapAndReleasedOutputsAreReused) {
  CreateRequest deep;
  deep.max_frame_depth = 100;
  CreateHandle(deep);
  EXPECT_TRUE(ReturnsCategory(
      driver_->Process(handle_, {std::vector<std::string>(65, "初始化"), 65}),
      Category::kInvalidInput));
  ASSERT_TRUE(ReturnsCategory(
      driver_->Process(handle_, {std::vector<std::string>(64, "初始化"), 64}),
      Category::kSuccess));
  EXPECT_EQ(driver_->PublishedOutputs(), 64u);
  driver_->ReleaseOutputs();
  void* consumed = std::exchange(handle_, nullptr);
  ASSERT_TRUE(ReturnsCategory(driver_->Destroy(consumed), Category::kSuccess));

  CreateRequest shallow;
  shallow.max_frame_depth = 1;
  CreateHandle(shallow);
  EXPECT_TRUE(
      ReturnsCategory(driver_->Process(handle_, {{"初始化", "初始化"}, 2}),
                      Category::kInvalidInput));
  for (int round = 0; round < 3; ++round) {
    SCOPED_TRACE(round);
    ASSERT_TRUE(ReturnsCategory(driver_->Process(handle_, {{"初始化"}, 1}),
                                Category::kSuccess));
    EXPECT_EQ(driver_->PublishedOutputs(), 1u);
    driver_->ReleaseOutputs();
  }
}

TEST_F(OperatorShellTest, ControlMapsRequestAndUnsupportedCommandStages) {
  CreateHandle();
  ControlRequest malformed{ControlKind::kJson, 1000003, "[]"};
  EXPECT_TRUE(ReturnsCategory(driver_->Control(handle_, malformed),
                              Category::kInvalidParam));
  ASSERT_GT(driver_->UndeclaredControlCommandCount(), 0u);
  for (size_t i = 0; i < driver_->UndeclaredControlCommandCount(); ++i) {
    SCOPED_TRACE(i);
    ControlRequest undeclared{ControlKind::kUndeclaredCommand, 2000000099, "{}",
                              i};
    EXPECT_TRUE(ReturnsCategory(driver_->Control(handle_, undeclared),
                                Category::kInvalidParam));
    EXPECT_NE(std::string(api::GetOperatorLastError())
                  .find("Unsupported or unknown ControlCommand"),
              std::string::npos);
  }
  ControlRequest unknown{ControlKind::kJson, 2000000099, "{}"};
  EXPECT_TRUE(ReturnsCategory(driver_->Control(handle_, unknown),
                              Category::kUnsupportedControl));
  EXPECT_NE(std::string(api::GetOperatorLastError()).find("internal code"),
            std::string::npos);
  ControlRequest valid{ControlKind::kUpdateRules, 0,
                       R"({"categories":{"CHANGED":["初始化"]}})"};
  ASSERT_TRUE(
      ReturnsCategory(driver_->Control(handle_, valid), Category::kSuccess));
  ASSERT_TRUE(ReturnsCategory(driver_->Process(handle_, {{"初始化"}, 1}),
                              Category::kSuccess));
  EXPECT_EQ(nlohmann::json::parse(driver_->OutputJson(0)).at("intent"),
            "CHANGED");
}

TEST_F(OperatorShellTest, DestroyWithOutstandingLeaseConsumesTheHandle) {
  CreateHandle();
  ASSERT_TRUE(ReturnsCategory(driver_->Process(handle_, {{"初始化"}, 1}),
                              Category::kSuccess));
  void* consumed = std::exchange(handle_, nullptr);
  EXPECT_TRUE(
      ReturnsCategory(driver_->Destroy(consumed), Category::kInvalidHandle));
  EXPECT_NE(std::string(api::GetOperatorLastError()).find("unreturned"),
            std::string::npos);
  EXPECT_TRUE(
      ReturnsCategory(driver_->Destroy(consumed), Category::kInvalidHandle));
  EXPECT_TRUE(ReturnsCategory(driver_->Process(consumed, {{"初始化"}, 1}),
                              Category::kInvalidHandle));
  ControlRequest control{ControlKind::kJson, 1000003, "{}"};
  EXPECT_TRUE(ReturnsCategory(driver_->Control(consumed, control),
                              Category::kInvalidHandle));
  // 过期的弱池 deleter 可以释放，但载荷已经回收。
  driver_->ReleaseOutputs();
}

TEST_F(OperatorShellTest, DestroyAfterReleasingOutputsSucceeds) {
  CreateHandle();
  EXPECT_TRUE(DestroySucceedsAfterOutputsAreReleased(*driver_, &handle_));
  EXPECT_EQ(handle_, nullptr);
}

TEST_F(OperatorShellTest,
       DeInitConsumesAllActiveHandlesAndLaterInitCanCreateAgain) {
  CreateHandle();
  void* second = nullptr;
  ASSERT_TRUE(
      ReturnsCategory(driver_->Create(&second, {}), Category::kSuccess));
  void* first = std::exchange(handle_, nullptr);
  EXPECT_TRUE(ReturnsCategory(driver_->DeInit(), Category::kSuccess));
  EXPECT_TRUE(
      ReturnsCategory(driver_->Destroy(first), Category::kInvalidHandle));
  EXPECT_TRUE(
      ReturnsCategory(driver_->Destroy(second), Category::kInvalidHandle));
  ASSERT_TRUE(ReturnsCategory(driver_->Init(), Category::kSuccess));
  CreateHandle();
}

TEST_F(OperatorShellTest, DiagnosticsBelongToTheCallingThread) {
  EXPECT_TRUE(
      ReturnsCategory(driver_->Process(nullptr, {}), Category::kInvalidHandle));
  const std::string main_error = api::GetOperatorLastError();
  auto other = std::async(std::launch::async, [driver = driver_.get()] {
    const std::string before = api::GetOperatorLastError();
    void* handle = nullptr;
    CreateRequest request;
    request.defect = CreateDefect::kNullParam;
    const int code = driver->Create(&handle, request);
    return std::make_tuple(before, code,
                           std::string(api::GetOperatorLastError()));
  });
  const auto [before, code, after] = other.get();
  EXPECT_TRUE(before.empty());
  EXPECT_TRUE(ReturnsCategory(code, Category::kInvalidHandle));
  EXPECT_NE(after.find("CreateParam"), std::string::npos);
  EXPECT_NE(main_error, after);
  EXPECT_EQ(std::string(api::GetOperatorLastError()), main_error);
}

TEST_F(OperatorShellTest,
       InputRejectionLogsOnceAndFilteringPreservesDiagnostic) {
  CreateHandle();
  ASSERT_EQ(AlgBase_setLogLevelByName(nullptr, E_ALG_BASE_LOG_LEVEL_WARNING),
            0);
  testing::internal::CaptureStderr();
  const int result = driver_->Process(handle_, {{}, 1});
  const auto record = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(ReturnsCategory(result, Category::kInvalidInput));
  EXPECT_NE(record.find("[Warning]"), std::string::npos);
  EXPECT_EQ(record.find("[Error]"), std::string::npos);
  EXPECT_EQ(std::count(record.begin(), record.end(), '\n'), 1);
  EXPECT_NE(record.find("operation=Operator_Process"), std::string::npos);
  EXPECT_NE(record.find("code=INVALID_INPUT(" +
                        std::to_string(error::kInvalidInput) + ")"),
            std::string::npos);
  EXPECT_NE(record.find("platform_code=" + std::to_string(RegisteredCode(
                                               Category::kInvalidInput))),
            std::string::npos);
  EXPECT_NE(record.find("stage=Runtime"), std::string::npos);
  EXPECT_EQ(record.find("Empty inputs NamedIoBatch"), std::string::npos);
  const std::string diagnostic = api::GetOperatorLastError();
  EXPECT_NE(diagnostic.find("INVALID_INPUT"), std::string::npos);
  EXPECT_NE(diagnostic.find("Empty inputs NamedIoBatch"), std::string::npos);
  EXPECT_EQ(driver_->PublishedOutputs(), 0u);

  ASSERT_EQ(AlgBase_setLogLevelByName(nullptr, E_ALG_BASE_LOG_LEVEL_FATAL), 0);
  testing::internal::CaptureStderr();
  const int filtered_result = driver_->Process(handle_, {{}, 1});
  const auto filtered_record = testing::internal::GetCapturedStderr();
  EXPECT_EQ(filtered_result, result);
  EXPECT_TRUE(filtered_record.empty());
  EXPECT_EQ(std::string(api::GetOperatorLastError()), diagnostic);
}

TEST_F(OperatorShellTest,
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
  CreateRequest request;
  request.model_path = directory;
  request.config = "deployment.conf";
  CreateHandle(request);
  ASSERT_EQ(AlgBase_setLogLevelByName(nullptr, E_ALG_BASE_LOG_LEVEL_VERBOSE),
            0);
  testing::internal::CaptureStderr();
  const int result = driver_->Process(handle_, {{"初始化"}, 1});
  const auto record = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(ReturnsCategory(result, Category::kUnknown));
  const auto error_record = record.find("[Error]");
  ASSERT_NE(error_record, std::string::npos);
  EXPECT_EQ(record.find("[Error]", error_record + 1), std::string::npos);
  EXPECT_EQ(record.find("[Warning]"), std::string::npos);
  EXPECT_NE(record.find("operation=Operator_Process"), std::string::npos);
  EXPECT_NE(record.find("code=RENDERED_OUTPUT_TOO_LONG(" +
                        std::to_string(error::kRenderedOutputTooLong) + ")"),
            std::string::npos);
  EXPECT_NE(record.find("platform_code=" +
                        std::to_string(RegisteredCode(Category::kUnknown))),
            std::string::npos);
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
  EXPECT_EQ(driver_->PublishedOutputs(), 0u);

  ASSERT_EQ(AlgBase_setLogLevelByName(nullptr, E_ALG_BASE_LOG_LEVEL_FATAL), 0);
  testing::internal::CaptureStderr();
  const int filtered_result = driver_->Process(handle_, {{"初始化"}, 1});
  const auto filtered_record = testing::internal::GetCapturedStderr();
  EXPECT_EQ(filtered_result, result);
  EXPECT_TRUE(filtered_record.empty());
  EXPECT_EQ(std::string(api::GetOperatorLastError()), diagnostic);
}

TEST_F(OperatorShellTest, BoundaryLogsOmitRequestContentButDiagnosticKeepsIt) {
  CreateHandle();
  // 未闭合的 JSON 字符串：解析异常的 what() 会携带 "last read" 原文片段。
  const std::string marker = "EF_SECRET_MARKER_7f3a";
  ControlRequest malformed{ControlKind::kUpdateRules, 0,
                           "{\"categories\":{\"" + marker + "\":[\"" + marker};
  ASSERT_EQ(AlgBase_setLogLevelByName(nullptr, E_ALG_BASE_LOG_LEVEL_VERBOSE),
            0);
  testing::internal::CaptureStderr();
  const int result = driver_->Control(handle_, malformed);
  const auto record = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(ReturnsCategory(result, Category::kInvalidParam));
  EXPECT_NE(record.find("operation=Operator_Control"), std::string::npos);
  EXPECT_NE(record.find("platform_code=" + std::to_string(result)),
            std::string::npos);
  EXPECT_EQ(record.find(marker), std::string::npos) << record;
  const std::string diagnostic = api::GetOperatorLastError();
  EXPECT_NE(diagnostic.find("Invalid control JSON"), std::string::npos);
  EXPECT_NE(diagnostic.find(marker), std::string::npos);
}

TEST_F(OperatorShellTest, ResolveConfigIoReturnsHostContractOrClearsOutput) {
  api::OperatorIoContract contract;
  char message[256] = {};
  ASSERT_TRUE(ReturnsCategory(
      api::ResolveOperatorConfigIo(".",
                                   "configs/pipeline_keyword_match_rules.conf",
                                   &contract, message, sizeof(message)),
      Category::kSuccess));
  ASSERT_EQ(contract.inputs.size(), 1u);
  ASSERT_EQ(contract.outputs.size(), 1u);
  EXPECT_EQ(contract.inputs[0].type, "keyword_in");
  EXPECT_EQ(contract.inputs[0].name, "keyword_match");
  EXPECT_FALSE(contract.inputs[0].type_name.empty());
  EXPECT_TRUE(contract.inputs[0].required);
  EXPECT_EQ(contract.outputs[0].type, "json_out");
  EXPECT_EQ(contract.outputs[0].name, "keyword_match");
  EXPECT_FALSE(contract.outputs[0].type_name.empty());

  EXPECT_TRUE(ReturnsCategory(
      api::ResolveOperatorConfigIo(".", "configs/x.conf", nullptr, message,
                                   sizeof(message)),
      Category::kInvalidParam));
  EXPECT_STREQ(message, "Null out");
  for (const auto& [root, config] :
       {std::pair<const char*, const char*>{"", "x.conf"},
        {".", nullptr},
        {".", "missing-operator-entry.conf"}}) {
    SCOPED_TRACE(config ? config : "(null)");
    api::OperatorIoContract cleared;
    cleared.inputs.push_back({});
    message[0] = '\0';
    EXPECT_TRUE(
        ReturnsCategory(api::ResolveOperatorConfigIo(root, config, &cleared,
                                                     message, sizeof(message)),
                        Category::kInvalidParam));
    EXPECT_TRUE(cleared.inputs.empty());
    EXPECT_TRUE(cleared.outputs.empty());
    EXPECT_NE(std::string(message), "");
    EXPECT_NE(std::string(api::GetOperatorLastError()), "");
  }
}

class OperatorShellFaultTest : public OperatorShellTest {
 protected:
  void SetUp() override {
    // 输出 binding 在 Init 与 Create 之前替换，Create 取得替换后的快照。
    driver_->SetOutputWriteFault(OutputWriteFault::kNone);
    OperatorShellTest::SetUp();
  }
};

TEST_F(OperatorShellFaultTest,
       BothExceptionBarriersReturnEveryLeaseAndPermitTheNextCall) {
  CreateHandle();
  const ProcessRequest rows{{"初始化", "初始化"}, 2};
  for (const auto fault :
       {OutputWriteFault::kStdException, OutputWriteFault::kUnknownException}) {
    const bool standard = fault == OutputWriteFault::kStdException;
    SCOPED_TRACE(standard ? "std::exception" : "unknown exception");
    driver_->SetOutputWriteFault(fault);
    EXPECT_TRUE(
        ReturnsCategory(driver_->Process(handle_, rows),
                        standard ? Category::kException : Category::kUnknown));
    EXPECT_EQ(driver_->OutputWrites(), 2u);
    EXPECT_EQ(driver_->ReturnedOutputLeases(), 2u);
    EXPECT_EQ(driver_->PublishedOutputs(), 0u);
    EXPECT_NE(std::string(api::GetOperatorLastError())
                  .find(standard ? kInjectedOutputWriteFault
                                 : "Unknown exception in Process"),
              std::string::npos);
    driver_->SetOutputWriteFault(OutputWriteFault::kNone);
    ASSERT_TRUE(
        ReturnsCategory(driver_->Process(handle_, rows), Category::kSuccess));
    EXPECT_EQ(driver_->PublishedOutputs(), 2u);
    driver_->ReleaseOutputs();
    EXPECT_EQ(driver_->ReturnedOutputLeases(), 2u);
  }
}

}  // namespace
}  // namespace llm_edgeflow::operator_shell_test

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "contracts/error_codes.h"
#include "edgeflow/log.h"
#include "edgeflow/operator/interface.h"
#include "tests/contract/operator_shell/operator_shell_checks.h"

// 用日志替身的阈值观察框架选择的等级，只检查框架消息和诊断文字。
// 平台输出前缀、换行和打印实现不属于这里的契约。
namespace llm_edgeflow::operator_shell_test {
namespace {
namespace api = operator_api;

size_t CountOperationRecords(const std::string& text, const char* operation) {
  const std::string marker = std::string("operation=") + operation;
  size_t count = 0;
  for (size_t offset = text.find(marker); offset != std::string::npos;
       offset = text.find(marker, offset + marker.size())) {
    ++count;
  }
  return count;
}

class MockBoundaryLogTest : public ::testing::Test {
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

TEST_F(MockBoundaryLogTest,
       InputRejectionLogsOnceAndFilteringPreservesDiagnostic) {
  CreateHandle();
  ASSERT_EQ(AlgBase_setLogLevelByName(nullptr, E_ALG_BASE_LOG_LEVEL_WARNING),
            0);
  testing::internal::CaptureStderr();
  const int result = driver_->Process(handle_, {{}, 1});
  const auto record = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(ReturnsCategory(result, Category::kInvalidInput));
  EXPECT_EQ(CountOperationRecords(record, "Operator_Process"), 1u);
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

  // WARNING 可见而 ERROR 不可见，确认框架选择了 WARNING。
  ASSERT_EQ(AlgBase_setLogLevelByName(nullptr, E_ALG_BASE_LOG_LEVEL_ERROR), 0);
  testing::internal::CaptureStderr();
  const int filtered_result = driver_->Process(handle_, {{}, 1});
  const auto filtered_record = testing::internal::GetCapturedStderr();
  EXPECT_EQ(filtered_result, result);
  EXPECT_TRUE(filtered_record.empty());
  EXPECT_EQ(std::string(api::GetOperatorLastError()), diagnostic);
}

TEST_F(MockBoundaryLogTest,
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
  ASSERT_EQ(AlgBase_setLogLevelByName(nullptr, E_ALG_BASE_LOG_LEVEL_ERROR), 0);
  testing::internal::CaptureStderr();
  const int result = driver_->Process(handle_, {{"初始化"}, 1});
  const auto record = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(ReturnsCategory(result, Category::kUnknown));
  EXPECT_EQ(CountOperationRecords(record, "Operator_Process"), 1u);
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

  // ERROR 可见而 FATAL 不可见，确认框架选择了 ERROR。
  ASSERT_EQ(AlgBase_setLogLevelByName(nullptr, E_ALG_BASE_LOG_LEVEL_FATAL), 0);
  testing::internal::CaptureStderr();
  const int filtered_result = driver_->Process(handle_, {{"初始化"}, 1});
  const auto filtered_record = testing::internal::GetCapturedStderr();
  EXPECT_EQ(filtered_result, result);
  EXPECT_TRUE(filtered_record.empty());
  EXPECT_EQ(std::string(api::GetOperatorLastError()), diagnostic);
}

TEST_F(MockBoundaryLogTest,
       BoundaryLogsOmitRequestContentButDiagnosticKeepsIt) {
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

TEST_F(MockBoundaryLogTest, DiagnosticsNameTheRejectedMockArgument) {
  CreateRequest null_param;
  null_param.defect = CreateDefect::kNullParam;
  EXPECT_TRUE(ReturnsCategory(driver_->Create(&handle_, null_param),
                              Category::kInvalidHandle));
  EXPECT_NE(std::string(api::GetOperatorLastError()).find("CreateParam"),
            std::string::npos);

  CreateHandle();
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

  char message[256] = {};
  EXPECT_TRUE(ReturnsCategory(
      api::ResolveOperatorConfigIo(".", "configs/x.conf", nullptr, message,
                                   sizeof(message)),
      Category::kInvalidParam));
  EXPECT_STREQ(message, "Null out");
}

class MockBoundaryFaultTest : public MockBoundaryLogTest {
 protected:
  void SetUp() override {
    // 输出 binding 在 Init 与 Create 之前替换，Create 取得替换后的快照。
    driver_->SetOutputWriteFault(OutputWriteFault::kNone);
    MockBoundaryLogTest::SetUp();
  }
};

TEST_F(MockBoundaryFaultTest, UnknownExceptionDiagnosticNamesTheEntry) {
  CreateHandle();
  driver_->SetOutputWriteFault(OutputWriteFault::kUnknownException);
  EXPECT_TRUE(
      ReturnsCategory(driver_->Process(handle_, {{"初始化", "初始化"}, 2}),
                      Category::kUnknown));
  EXPECT_NE(std::string(api::GetOperatorLastError())
                .find("Unknown exception in Process"),
            std::string::npos);
}

}  // namespace
}  // namespace llm_edgeflow::operator_shell_test

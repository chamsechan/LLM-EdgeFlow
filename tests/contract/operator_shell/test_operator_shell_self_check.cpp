#include <gtest/gtest.h>

#include <memory>

#include "tests/contract/operator_shell/operator_shell_checks.h"

// 违约自测：在测试中包装函数表制造违约，证明场景检查能够识别。
namespace llm_edgeflow::operator_shell_test {
namespace {

class OperatorShellSelfCheckTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(ReturnsCategory(driver_->Init(), Category::kSuccess));
    ASSERT_TRUE(
        ReturnsCategory(driver_->Create(&handle_, {}), Category::kSuccess));
  }
  void TearDown() override {
    driver_->SetViolation(Violation::kNone);
    driver_->ReleaseOutputs();
    driver_->ReleaseViolationReferences();
    if (handle_) {
      EXPECT_TRUE(
          ReturnsCategory(driver_->Destroy(handle_), Category::kSuccess));
    }
    EXPECT_TRUE(ReturnsCategory(driver_->DeInit(), Category::kSuccess));
  }
  std::unique_ptr<OperatorShellDriver> driver_ = MakeOperatorShellDriver();
  void* handle_ = nullptr;
};

TEST_F(OperatorShellSelfCheckTest, PassedThroughInternalCodeIsDetected) {
  ASSERT_TRUE(RejectedBatchesPublishNothing(*driver_, handle_));
  driver_->SetViolation(Violation::kLeakInternalCode);
  const auto result = RejectedBatchesPublishNothing(*driver_, handle_);
  EXPECT_FALSE(result);
  EXPECT_NE(std::string(result.message()).find("not a registered public code"),
            std::string::npos)
      << result.message();
}

TEST_F(OperatorShellSelfCheckTest, OutputWrittenOnFailureIsDetected) {
  driver_->SetViolation(Violation::kWriteOutputOnFailure);
  const auto result = RejectedBatchesPublishNothing(*driver_, handle_);
  driver_->ReleaseOutputs();
  EXPECT_FALSE(result);
  EXPECT_NE(std::string(result.message()).find("after a failed Process"),
            std::string::npos)
      << result.message();
}

TEST_F(OperatorShellSelfCheckTest, RetainedOutputReferenceIsDetected) {
  // 同步步骤：单行 Process 后包装器保留一份输出引用，套件释放自己的引用后
  // Destroy；场景检查消费句柄，随后释放包装器引用再报告结果。
  driver_->SetViolation(Violation::kRetainOutputReference);
  const auto result =
      DestroySucceedsAfterOutputsAreReleased(*driver_, &handle_);
  EXPECT_EQ(handle_, nullptr);
  driver_->SetViolation(Violation::kNone);
  driver_->ReleaseViolationReferences();
  EXPECT_FALSE(result);
  EXPECT_NE(std::string(result.message()).find("[Destroy]"), std::string::npos)
      << result.message();
}

}  // namespace
}  // namespace llm_edgeflow::operator_shell_test

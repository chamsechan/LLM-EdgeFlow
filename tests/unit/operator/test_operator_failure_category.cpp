#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "adapter/operator/operator_failure_category.h"
#include "contracts/error_codes.h"

namespace llm_edgeflow {
namespace {
using Category = OperatorFailureCategory;
using Stage = OperatorFailureStage;

constexpr Stage kAllStages[] = {
    Stage::kCreatePreparation, Stage::kProcessExecution,
    Stage::kControlRequest,    Stage::kControlUnsupported,
    Stage::kControlExecution,  Stage::kRuntime};

// 不属于已声明内部码、也不等于任一平台码的值。
constexpr int kUndeclared = -35;

struct Row {
  Stage stage;
  int code;
  Category expected;
};

TEST(OperatorFailureCategoryTest, SuccessIsCheckedBeforeEveryStage) {
  for (const auto stage : kAllStages) {
    SCOPED_TRACE(OperatorFailureStageName(stage));
    EXPECT_EQ(ClassifyOperatorFailure(stage, error::kOk), Category::kSuccess);
  }
}

TEST(OperatorFailureCategoryTest, CreatePreparationKeepsDedicatedBranches) {
  const std::vector<Row> rows{
      {Stage::kCreatePreparation, error::kUnsupported,
       Category::kUnsupportedBiz},
      {Stage::kCreatePreparation, error::kRegistryConflict,
       Category::kRegistryConflict},
      {Stage::kCreatePreparation, error::kInternalException,
       Category::kException},
      {Stage::kCreatePreparation, error::kUnknownException, Category::kUnknown},
      {Stage::kCreatePreparation, error::kValidationFailed,
       Category::kInvalidParam},
      {Stage::kCreatePreparation, error::kInvalidState,
       Category::kInvalidParam},
      {Stage::kCreatePreparation, error::kAllocationFailed,
       Category::kInvalidParam},
      {Stage::kCreatePreparation, kUndeclared, Category::kInvalidParam}};
  for (const auto& row : rows) {
    SCOPED_TRACE(row.code);
    EXPECT_EQ(ClassifyOperatorFailure(row.stage, row.code), row.expected);
  }
}

TEST(OperatorFailureCategoryTest, WholeStageFallbacksIgnoreTheInternalCode) {
  for (const int code :
       {error::kInvalidInput, error::kUnsupported, error::kInternalException,
        error::kUnknownException, error::kUnsupportedControl, kUndeclared}) {
    SCOPED_TRACE(code);
    EXPECT_EQ(ClassifyOperatorFailure(Stage::kProcessExecution, code),
              Category::kUnknown);
    EXPECT_EQ(ClassifyOperatorFailure(Stage::kControlExecution, code),
              Category::kUnknown);
    EXPECT_EQ(ClassifyOperatorFailure(Stage::kControlRequest, code),
              Category::kInvalidParam);
    EXPECT_EQ(ClassifyOperatorFailure(Stage::kControlUnsupported, code),
              Category::kUnsupportedControl);
  }
}

TEST(OperatorFailureCategoryTest, RuntimeMapsEachDeclaredCodeAndFallsBack) {
  const std::vector<Row> rows{
      {Stage::kRuntime, error::kInvalidState, Category::kInvalidHandle},
      {Stage::kRuntime, error::kInvalidArgument, Category::kInvalidParam},
      {Stage::kRuntime, error::kInvalidOutputAllocation,
       Category::kInvalidParam},
      {Stage::kRuntime, error::kInvalidInput, Category::kInvalidInput},
      {Stage::kRuntime, error::kMissingInput, Category::kInvalidInput},
      {Stage::kRuntime, error::kInvalidOutput, Category::kOutputCapacity},
      {Stage::kRuntime, error::kOutputCountMismatch, Category::kOutputCapacity},
      {Stage::kRuntime, error::kOutputProvenanceMismatch,
       Category::kOutputCapacity},
      {Stage::kRuntime, error::kAllocationFailed, Category::kOutputCapacity},
      {Stage::kRuntime, error::kPoolMissing, Category::kOutputCapacity},
      {Stage::kRuntime, error::kPoolClosed, Category::kOutputCapacity},
      {Stage::kRuntime, error::kPoolCorrupt, Category::kOutputCapacity},
      {Stage::kRuntime, error::kUnsupported, Category::kUnsupportedBiz},
      {Stage::kRuntime, error::kUnknownConverter, Category::kUnsupportedBiz},
      {Stage::kRuntime, error::kRegistryConflict, Category::kRegistryConflict},
      {Stage::kRuntime, error::kUnsupportedControl,
       Category::kUnsupportedControl},
      {Stage::kRuntime, error::kInternalException, Category::kException},
      {Stage::kRuntime, error::kUnknownException, Category::kUnknown},
      {Stage::kRuntime, error::kInferenceFailed, Category::kUnknown},
      {Stage::kRuntime, kUndeclared, Category::kUnknown}};
  for (const auto& row : rows) {
    SCOPED_TRACE(row.code);
    EXPECT_EQ(ClassifyOperatorFailure(row.stage, row.code), row.expected);
  }
}

TEST(OperatorFailureCategoryTest,
     RawValuesEqualToPlatformCodesUseStageFallback) {
  // 内部码偶然等于某个平台码时，仍按所在阶段兜底，不透传为对应类别。
  for (const int raw : {-1, -2, -3, -4, -5, -6, -7, -99, -100}) {
    SCOPED_TRACE(raw);
    EXPECT_EQ(ClassifyOperatorFailure(Stage::kRuntime, raw),
              Category::kUnknown);
    EXPECT_EQ(ClassifyOperatorFailure(Stage::kCreatePreparation, raw),
              Category::kInvalidParam);
  }
}

TEST(OperatorFailureCategoryTest,
     CallerRejectionsLogAsWarningsExceptResources) {
  for (const auto category :
       {Category::kInvalidHandle, Category::kInvalidParam,
        Category::kInvalidInput, Category::kOutputCapacity,
        Category::kUnsupportedBiz, Category::kUnsupportedControl}) {
    EXPECT_TRUE(IsRejectedOperatorRequest(category, error::kInvalidInput));
    for (const int code :
         {error::kAllocationFailed, error::kPoolCorrupt, error::kPoolMissing,
          error::kModelMaterializationFailed, error::kNodeCreateFailed,
          error::kNodeInitFailed}) {
      SCOPED_TRACE(code);
      EXPECT_FALSE(IsRejectedOperatorRequest(category, code));
    }
  }
  for (const auto category : {Category::kSuccess, Category::kRegistryConflict,
                              Category::kException, Category::kUnknown}) {
    EXPECT_FALSE(IsRejectedOperatorRequest(category, error::kInvalidInput));
  }
}

TEST(OperatorFailureCategoryTest, ControlStagesAndDiagnosticsKeepTheCause) {
  EXPECT_EQ(ControlFailureToOperatorStage(ControlFailureStage::kRequest),
            Stage::kControlRequest);
  EXPECT_EQ(ControlFailureToOperatorStage(ControlFailureStage::kUnsupported),
            Stage::kControlUnsupported);
  EXPECT_EQ(ControlFailureToOperatorStage(ControlFailureStage::kNode),
            Stage::kControlExecution);
  EXPECT_EQ(ControlFailureToOperatorStage(ControlFailureStage::kNone),
            Stage::kRuntime);
  EXPECT_EQ(DescribeOperatorFailure(Stage::kControlExecution,
                                    error::kInvalidInput, "literal cause"),
            "Control node update failed with internal code " +
                std::to_string(error::kInvalidInput) +
                " (INVALID_INPUT): literal cause");
}

}  // namespace
}  // namespace llm_edgeflow

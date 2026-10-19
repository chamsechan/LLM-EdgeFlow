#include "adapter/operator/operator_failure_category.h"

#include "contracts/error_codes.h"

namespace llm_edgeflow {

const char* OperatorFailureStageName(OperatorFailureStage stage) noexcept {
  switch (stage) {
    case OperatorFailureStage::kCreatePreparation:
      return "Create preparation";
    case OperatorFailureStage::kProcessExecution:
      return "Pipeline execution";
    case OperatorFailureStage::kControlRequest:
      return "Control request";
    case OperatorFailureStage::kControlUnsupported:
      return "Control command";
    case OperatorFailureStage::kControlExecution:
      return "Control node update";
    case OperatorFailureStage::kRuntime:
      break;
  }
  return "Runtime";
}

OperatorFailureStage ControlFailureToOperatorStage(
    ControlFailureStage stage) noexcept {
  switch (stage) {
    case ControlFailureStage::kRequest:
      return OperatorFailureStage::kControlRequest;
    case ControlFailureStage::kUnsupported:
      return OperatorFailureStage::kControlUnsupported;
    case ControlFailureStage::kNode:
      return OperatorFailureStage::kControlExecution;
    case ControlFailureStage::kNone:
      break;
  }
  return OperatorFailureStage::kRuntime;
}

OperatorFailureCategory ClassifyOperatorFailure(OperatorFailureStage stage,
                                                int internal_code) noexcept {
  using Category = OperatorFailureCategory;
  if (internal_code == error::kOk) return Category::kSuccess;
  switch (stage) {
    case OperatorFailureStage::kCreatePreparation:
      switch (internal_code) {
        case error::kUnsupported:
          return Category::kUnsupportedBiz;
        case error::kRegistryConflict:
          return Category::kRegistryConflict;
        case error::kInternalException:
          return Category::kException;
        case error::kUnknownException:
          return Category::kUnknown;
        default:
          return Category::kInvalidParam;
      }
    case OperatorFailureStage::kProcessExecution:
    case OperatorFailureStage::kControlExecution:
      return Category::kUnknown;
    case OperatorFailureStage::kControlRequest:
      return Category::kInvalidParam;
    case OperatorFailureStage::kControlUnsupported:
      return Category::kUnsupportedControl;
    case OperatorFailureStage::kRuntime:
      break;
  }
  switch (internal_code) {
    case error::kInvalidState:
      return Category::kInvalidHandle;
    case error::kInvalidArgument:
    case error::kInvalidOutputAllocation:
      return Category::kInvalidParam;
    case error::kInvalidInput:
    case error::kMissingInput:
      return Category::kInvalidInput;
    case error::kInvalidOutput:
    case error::kOutputCountMismatch:
    case error::kOutputProvenanceMismatch:
    case error::kAllocationFailed:
    case error::kPoolMissing:
    case error::kPoolClosed:
    case error::kPoolCorrupt:
      return Category::kOutputCapacity;
    case error::kUnsupported:
    case error::kUnknownConverter:
      return Category::kUnsupportedBiz;
    case error::kRegistryConflict:
      return Category::kRegistryConflict;
    case error::kUnsupportedControl:
      return Category::kUnsupportedControl;
    case error::kInternalException:
      return Category::kException;
    default:
      return Category::kUnknown;
  }
}

bool IsRejectedOperatorRequest(OperatorFailureCategory category,
                               int internal_code) noexcept {
  switch (category) {
    case OperatorFailureCategory::kInvalidHandle:
    case OperatorFailureCategory::kInvalidParam:
    case OperatorFailureCategory::kInvalidInput:
    case OperatorFailureCategory::kOutputCapacity:
    case OperatorFailureCategory::kUnsupportedBiz:
    case OperatorFailureCategory::kUnsupportedControl:
      break;
    default:
      return false;
  }
  return internal_code != error::kAllocationFailed &&
         internal_code != error::kPoolCorrupt &&
         internal_code != error::kPoolMissing &&
         internal_code != error::kModelMaterializationFailed &&
         internal_code != error::kNodeCreateFailed &&
         internal_code != error::kNodeInitFailed;
}

std::string DescribeOperatorFailure(OperatorFailureStage stage,
                                    int internal_code,
                                    std::string_view detail) {
  return std::string(OperatorFailureStageName(stage)) +
         " failed with internal code " + std::to_string(internal_code) + " (" +
         ErrorCodeName(internal_code) + "): " + std::string(detail);
}

}  // namespace llm_edgeflow

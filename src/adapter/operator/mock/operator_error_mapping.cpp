#include "adapter/operator/mock/operator_error_mapping.h"

#include "contracts/error_codes.h"
#include "platform_mock/error_codes.h"

namespace llm_edgeflow {

namespace {

const char* StageName(OperatorFailureStage stage) noexcept {
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

}  // namespace

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

int PublicFailureCode(OperatorFailureStage stage, int internal_code) noexcept {
  if (internal_code == error::kOk) return COMPANY_ALG_SUCCESS;
  switch (stage) {
    case OperatorFailureStage::kCreatePreparation:
      switch (internal_code) {
        case error::kUnsupported:
          return COMPANY_ALG_ERR_UNSUPPORTED_BIZ;
        case error::kRegistryConflict:
          return COMPANY_ALG_ERR_REGISTRY_CONFLICT;
        case error::kInternalException:
          return COMPANY_ALG_ERR_EXCEPTION;
        case error::kUnknownException:
          return COMPANY_ALG_ERR_UNKNOWN;
        default:
          return COMPANY_ALG_ERR_INVALID_PARAM;
      }
    case OperatorFailureStage::kProcessExecution:
    case OperatorFailureStage::kControlExecution:
      return COMPANY_ALG_ERR_UNKNOWN;
    case OperatorFailureStage::kControlRequest:
      return COMPANY_ALG_ERR_INVALID_PARAM;
    case OperatorFailureStage::kControlUnsupported:
      return COMPANY_ALG_ERR_UNSUPPORTED_CONTROL;
    case OperatorFailureStage::kRuntime:
      break;
  }
  switch (internal_code) {
    case error::kInvalidState:
      return COMPANY_ALG_ERR_INVALID_HANDLE;
    case error::kInvalidArgument:
    case error::kInvalidOutputAllocation:
      return COMPANY_ALG_ERR_INVALID_PARAM;
    case error::kInvalidInput:
    case error::kMissingInput:
      return COMPANY_ALG_ERR_INVALID_INPUT;
    case error::kInvalidOutput:
    case error::kOutputCountMismatch:
    case error::kOutputProvenanceMismatch:
    case error::kAllocationFailed:
    case error::kPoolMissing:
    case error::kPoolClosed:
    case error::kPoolCorrupt:
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    case error::kUnsupported:
    case error::kUnknownConverter:
      return COMPANY_ALG_ERR_UNSUPPORTED_BIZ;
    case error::kRegistryConflict:
      return COMPANY_ALG_ERR_REGISTRY_CONFLICT;
    case error::kUnsupportedControl:
      return COMPANY_ALG_ERR_UNSUPPORTED_CONTROL;
    case error::kInternalException:
      return COMPANY_ALG_ERR_EXCEPTION;
    default:
      return COMPANY_ALG_ERR_UNKNOWN;
  }
}

std::string DescribeOperatorFailure(OperatorFailureStage stage,
                                    int internal_code,
                                    std::string_view detail) {
  return std::string(StageName(stage)) + " failed with internal code " +
         std::to_string(internal_code) + " (" + ErrorCodeName(internal_code) +
         "): " + std::string(detail);
}

}  // namespace llm_edgeflow

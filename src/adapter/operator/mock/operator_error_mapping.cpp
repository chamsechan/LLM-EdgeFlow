#include "adapter/operator/mock/operator_error_mapping.h"

#include "platform_mock/error_codes.h"

namespace llm_edgeflow {

int PlatformFailureCode(OperatorFailureCategory category) noexcept {
  switch (category) {
    case OperatorFailureCategory::kSuccess:
      return COMPANY_ALG_SUCCESS;
    case OperatorFailureCategory::kInvalidHandle:
      return COMPANY_ALG_ERR_INVALID_HANDLE;
    case OperatorFailureCategory::kInvalidParam:
      return COMPANY_ALG_ERR_INVALID_PARAM;
    case OperatorFailureCategory::kInvalidInput:
      return COMPANY_ALG_ERR_INVALID_INPUT;
    case OperatorFailureCategory::kOutputCapacity:
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    case OperatorFailureCategory::kUnsupportedBiz:
      return COMPANY_ALG_ERR_UNSUPPORTED_BIZ;
    case OperatorFailureCategory::kRegistryConflict:
      return COMPANY_ALG_ERR_REGISTRY_CONFLICT;
    case OperatorFailureCategory::kUnsupportedControl:
      return COMPANY_ALG_ERR_UNSUPPORTED_CONTROL;
    case OperatorFailureCategory::kException:
      return COMPANY_ALG_ERR_EXCEPTION;
    case OperatorFailureCategory::kUnknown:
      break;
  }
  return COMPANY_ALG_ERR_UNKNOWN;
}

}  // namespace llm_edgeflow

#include "adapter/operator/operator_error_mapping.h"

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
    case OperatorFailureStage::kControlRuntime:
      break;
  }
  return "Control runtime";
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
  return OperatorFailureStage::kControlRuntime;
}

int PublicFailureCode(OperatorFailureStage stage, int internal_code) noexcept {
  switch (stage) {
    case OperatorFailureStage::kCreatePreparation:
      // Adapter-owned categories keep their meaning; every other preparation
      // failure is an invalid creation parameter or configuration.
      switch (internal_code) {
        case COMPANY_ALG_ERR_UNSUPPORTED_BIZ:
        case COMPANY_ALG_ERR_REGISTRY_CONFLICT:
        case COMPANY_ALG_ERR_EXCEPTION:
        case COMPANY_ALG_ERR_UNKNOWN:
          return internal_code;
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
    case OperatorFailureStage::kControlRuntime:
      break;
  }
  // Runtime guards and exception barriers already return public codes.
  return internal_code;
}

std::string DescribeOperatorFailure(OperatorFailureStage stage,
                                    int internal_code,
                                    std::string_view detail) {
  return std::string(StageName(stage)) + " failed with internal code " +
         std::to_string(internal_code) + ": " + std::string(detail);
}

}  // namespace llm_edgeflow

#pragma once

#include <string>
#include <string_view>

#include "core/pipeline.h"

namespace llm_edgeflow {

// Failures that originate below the Operator facade. Their internal Pipeline,
// Node and Model codes are reported through GetOperatorLastError() only; the
// host receives the public code of the failure stage.
enum class OperatorFailureStage {
  kCreatePreparation,   // configuration, deployment, model and backend loading
  kProcessExecution,    // a Node or Model failed in Pipeline::Execute
  kControlRequest,      // Control envelope, target or payload was rejected
  kControlUnsupported,  // no Node declares or handles the command
  kControlExecution,    // a Node rejected or failed to apply the update
  kControlRuntime,      // no ready runtime, or an exception barrier fired
};

OperatorFailureStage ControlFailureToOperatorStage(
    ControlFailureStage stage) noexcept;

int PublicFailureCode(OperatorFailureStage stage, int internal_code) noexcept;

std::string DescribeOperatorFailure(OperatorFailureStage stage,
                                    int internal_code, std::string_view detail);

}  // namespace llm_edgeflow

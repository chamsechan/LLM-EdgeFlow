#pragma once

#include <string>

#include "core/diagnostic_code.h"

namespace llm_edgeflow {

/**
 * @brief 轻量级结构化诊断信息 (PipelineDiagnostic)
 */
struct PipelineDiagnostic {
  DiagnosticCode code = DiagnosticCode::kOk;
  std::string path;
  std::string message;

  bool IsOk() const { return code == DiagnosticCode::kOk; }

  void Clear() {
    code = DiagnosticCode::kOk;
    path.clear();
    message.clear();
  }
};

}  // namespace llm_edgeflow

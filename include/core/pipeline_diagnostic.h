#pragma once

#include <string>

#include "contracts/error_codes.h"

namespace llm_edgeflow {

/**
 * @brief 轻量级结构化诊断信息 (PipelineDiagnostic)
 */
struct PipelineDiagnostic {
  int code = error::kOk;
  std::string path;
  std::string message;

  bool IsOk() const { return code == error::kOk; }

  void Clear() {
    code = error::kOk;
    path.clear();
    message.clear();
  }
};

}  // namespace llm_edgeflow

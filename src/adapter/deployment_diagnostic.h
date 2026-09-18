#pragma once

#include <optional>
#include <string>

#include "core/diagnostic_code.h"
#include "core/pipeline_diagnostic.h"

namespace llm_edgeflow {

/**
 * @brief 轻量级部署诊断载体 (RFC-0062)
 *
 * 用于在 Integration 内部及工具调用面跨函数传递结构化错误 (code, path, message,
 * legacy_status).
 */
struct DeploymentDiagnostic {
  std::string code;  // Integration 错误码，或 DiagnosticCodeName 的结果
  std::string path;  // 原始完整文档的 RFC 6901 JSON Pointer
  std::string message;
  int legacy_status = -2;
  std::optional<PipelineDiagnostic> pipeline_diagnostic;

  void Clear() {
    code.clear();
    path.clear();
    message.clear();
    legacy_status = -2;
    pipeline_diagnostic.reset();
  }
};

}  // namespace llm_edgeflow

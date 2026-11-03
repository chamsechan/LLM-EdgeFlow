#pragma once

#include <string>

#include "contracts/error_codes.h"

namespace llm_edgeflow {

/**
 * @brief 轻量级部署诊断载体
 *
 * 在 Integration 内部及工具调用之间传递结构化错误（code、path、message）。
 */
struct DeploymentDiagnostic {
  int code = error::kOk;  // 框架统一错误码
  std::string path;       // 原始完整文档的 RFC 6901 JSON Pointer
  std::string message;

  void Clear() {
    code = error::kOk;
    path.clear();
    message.clear();
  }
};

}  // namespace llm_edgeflow

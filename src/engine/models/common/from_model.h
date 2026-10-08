#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "contracts/diagnostic.h"
#include "edgeflow/log.h"

namespace llm_edgeflow {

// 取"可从模型读取"的参数的最终值：
// - 配置写了：用配置值；模型也给出且两者不同时报错；
// - 配置没写：用模型给出的值；模型给不出时用 fallback；
//   没有 fallback 时报错，提示在参数中填写。
// 最终值写入 Create 日志。
//
// 只用于模型文件真正提供了读取接口的参数（例如 ONNX 张量的固定形状）。
inline bool ResolveFromModel(const char* name,
                             std::optional<int64_t> configured,
                             std::optional<int64_t> detected,
                             std::optional<int64_t> fallback, int64_t* value,
                             std::string* diagnostic) {
  if (!value) {
    SetDiagnosticNoexcept(diagnostic,
                          "ResolveFromModel output pointer is null");
    return false;
  }
  const char* source = "configured";
  int64_t resolved = 0;
  if (configured.has_value()) {
    if (detected.has_value() && *detected != *configured) {
      SetDiagnosticNoexcept(diagnostic,
                            std::string("Parameter '") + name + "' is " +
                                std::to_string(*configured) +
                                " but the model reports " +
                                std::to_string(*detected) +
                                "; remove it or set it to the model's value");
      return false;
    }
    resolved = *configured;
  } else if (detected.has_value()) {
    resolved = *detected;
    source = "read from model";
  } else if (fallback.has_value()) {
    resolved = *fallback;
    source = "fallback";
  } else {
    SetDiagnosticNoexcept(diagnostic,
                          std::string("Parameter '") + name +
                              "' cannot be read from the model; set it in the "
                              "parameters");
    return false;
  }
  ALG_LOG_INFO("[Model] %s = %lld (%s)\n", name,
               static_cast<long long>(resolved), source);
  *value = resolved;
  return true;
}

}  // namespace llm_edgeflow

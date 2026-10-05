#pragma once

#include <string>
#include <string_view>

namespace llm_edgeflow {

// 诊断信息分配失败不得把可恢复错误变成 terminate。catch 块中应传入
// exception.what() 或字面量，而不是临时 string。
inline void SetDiagnosticNoexcept(std::string* output,
                                  std::string_view message) noexcept {
  if (!output) return;
  try {
    output->assign(message);
  } catch (...) {
    output->clear();
  }
}

}  // namespace llm_edgeflow

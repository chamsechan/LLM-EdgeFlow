#pragma once

#include <string>
#include <string_view>

namespace llm_edgeflow {

// Diagnostic allocation must not turn a recoverable failure into terminate.
// Pass exception.what() or a literal in catch blocks, not a temporary string.
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

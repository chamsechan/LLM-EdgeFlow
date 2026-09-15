#include "core/diagnostic_code.h"

namespace llm_edgeflow {

const char* DiagnosticCodeName(DiagnosticCode code) noexcept {
  switch (code) {
#define LLM_EDGEFLOW_CASE(name, str) \
  case DiagnosticCode::name:         \
    return str;
    LLM_EDGEFLOW_DIAGNOSTIC_CODES(LLM_EDGEFLOW_CASE)
#undef LLM_EDGEFLOW_CASE
  }
  return "UNKNOWN";
}

}  // namespace llm_edgeflow

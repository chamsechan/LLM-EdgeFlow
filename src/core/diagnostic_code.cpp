#include "core/diagnostic_code.h"

#include "contracts/config_schema_validation.h"

namespace llm_edgeflow {

DiagnosticCode ConfigFieldDiagnosticCode(ConfigFieldErrorKind kind) noexcept {
  switch (kind) {
    case ConfigFieldErrorKind::kUnknownField:
      return DiagnosticCode::kUnknownConfigField;
    case ConfigFieldErrorKind::kMissingField:
      return DiagnosticCode::kMissingConfigField;
    case ConfigFieldErrorKind::kOutOfRange:
    case ConfigFieldErrorKind::kNonFinite:
      return DiagnosticCode::kConfigFieldRange;
    case ConfigFieldErrorKind::kInvalidEnum:
      return DiagnosticCode::kConfigFieldEnum;
    case ConfigFieldErrorKind::kNotAnObject:
    case ConfigFieldErrorKind::kTypeMismatch:
      return DiagnosticCode::kConfigFieldType;
  }
  return DiagnosticCode::kConfigFieldType;
}

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

#include "core/remediation_cause.h"

namespace llm_edgeflow {

const char* RemediationCauseName(RemediationCause cause) noexcept {
  switch (cause) {
#define LLM_EDGEFLOW_CAUSE_CASE(name, str) \
  case RemediationCause::name:             \
    return str;
    LLM_EDGEFLOW_REMEDIATION_CAUSES(LLM_EDGEFLOW_CAUSE_CASE)
#undef LLM_EDGEFLOW_CAUSE_CASE
  }
  return "UNKNOWN";
}

}  // namespace llm_edgeflow

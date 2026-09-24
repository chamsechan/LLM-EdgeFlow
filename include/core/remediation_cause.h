#pragma once

namespace llm_edgeflow {

#define LLM_EDGEFLOW_REMEDIATION_CAUSES(X)                  \
  X(kUnknownConfigField, "unknown_config_field")            \
  X(kMissingConfigField, "missing_config_field")            \
  X(kInvalidConfigValue, "invalid_config_value")            \
  X(kUnknownModelReference, "unknown_model_reference")      \
  X(kModelCapabilityMismatch, "model_capability_mismatch")  \
  X(kPortTypeMismatch, "port_type_mismatch")                \
  X(kNoCompatibleInputSource, "no_compatible_input_source") \
  X(kDuplicateDependency, "duplicate_dependency")           \
  X(kUnknownDependency, "unknown_dependency")               \
  X(kMissingBizOutput, "missing_biz_output")                \
  X(kPortFlowMismatch, "port_flow_mismatch")

enum class RemediationCause {
#define LLM_EDGEFLOW_DEF_CAUSE(name, str) name,
  LLM_EDGEFLOW_REMEDIATION_CAUSES(LLM_EDGEFLOW_DEF_CAUSE)
#undef LLM_EDGEFLOW_DEF_CAUSE
};

const char* RemediationCauseName(RemediationCause cause) noexcept;

}  // namespace llm_edgeflow

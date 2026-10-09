#pragma once

namespace llm_edgeflow {

#define LLM_EDGEFLOW_REMEDIATION_CAUSES(X)                  \
  X(kUnknownConfigField, "unknown_config_field")            \
  X(kMissingConfigField, "missing_config_field")            \
  X(kInvalidConfigValue, "invalid_config_value")            \
  X(kUnknownModelReference, "unknown_model_reference")      \
  X(kModelTypeMismatch, "model_type_mismatch")              \
  X(kPortTypeMismatch, "port_type_mismatch")                \
  X(kNoCompatibleInputSource, "no_compatible_input_source") \
  X(kDuplicateDependency, "duplicate_dependency")           \
  X(kUnknownDependency, "unknown_dependency")               \
  X(kMissingOutputProducer, "missing_output_producer")      \
  X(kPortFlowMismatch, "port_flow_mismatch")                \
  X(kUnknownNodeType, "unknown_node_type")                  \
  X(kUnknownModelType, "unknown_model_type")                \
  X(kBackendProtocolMismatch, "backend_protocol_mismatch")  \
  X(kUnknownBackend, "unknown_backend")

enum class RemediationCause {
#define LLM_EDGEFLOW_DEF_CAUSE(name, str) name,
  LLM_EDGEFLOW_REMEDIATION_CAUSES(LLM_EDGEFLOW_DEF_CAUSE)
#undef LLM_EDGEFLOW_DEF_CAUSE
};

const char* RemediationCauseName(RemediationCause cause) noexcept;

}  // namespace llm_edgeflow

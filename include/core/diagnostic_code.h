#pragma once

namespace llm_edgeflow {

#define LLM_EDGEFLOW_DIAGNOSTIC_CODES(X)                         \
  X(kOk, "OK")                                                   \
  X(kJsonParse, "JSON_PARSE")                                    \
  X(kConfigFileOpen, "CONFIG_FILE_OPEN")                         \
  X(kRootType, "ROOT_TYPE")                                      \
  X(kUnknownField, "UNKNOWN_FIELD")                              \
  X(kMissingField, "MISSING_FIELD")                              \
  X(kFieldType, "FIELD_TYPE")                                    \
  X(kFieldRange, "FIELD_RANGE")                                  \
  X(kInvalidCombination, "INVALID_COMBINATION")                  \
  X(kDuplicateModelName, "DUPLICATE_MODEL_NAME")                 \
  X(kDuplicateNodeName, "DUPLICATE_NODE_NAME")                   \
  X(kInvalidNodeName, "INVALID_NODE_NAME")                       \
  X(kUnknownNodeReference, "UNKNOWN_NODE_REFERENCE")             \
  X(kUnknownPortReference, "UNKNOWN_PORT_REFERENCE")             \
  X(kPortTypeMismatch, "PORT_TYPE_MISMATCH")                     \
  X(kUnknownNodeType, "UNKNOWN_NODE_TYPE")                       \
  X(kUnknownModelType, "UNKNOWN_MODEL_TYPE")                     \
  X(kUnknownBackend, "UNKNOWN_BACKEND")                          \
  X(kModelTypeMismatch, "MODEL_TYPE_MISMATCH")                   \
  X(kBackendProtocolMismatch, "BACKEND_PROTOCOL_MISMATCH")       \
  X(kInvalidDependency, "INVALID_DEPENDENCY")                    \
  X(kDuplicateDependency, "DUPLICATE_DEPENDENCY")                \
  X(kDagCycle, "DAG_CYCLE")                                      \
  X(kRegistryConflict, "REGISTRY_CONFLICT")                      \
  X(kUnknownConfigField, "UNKNOWN_CONFIG_FIELD")                 \
  X(kMissingConfigField, "MISSING_CONFIG_FIELD")                 \
  X(kConfigFieldType, "CONFIG_FIELD_TYPE")                       \
  X(kConfigFieldRange, "CONFIG_FIELD_RANGE")                     \
  X(kConfigFieldEnum, "CONFIG_FIELD_ENUM")                       \
  X(kUnusedModel, "UNUSED_MODEL")                                \
  X(kUnknownModelReference, "UNKNOWN_MODEL_REFERENCE")           \
  X(kMissingInputProducer, "MISSING_INPUT_PRODUCER")             \
  X(kDuplicatePortProducer, "DUPLICATE_PORT_PRODUCER")           \
  X(kMissingOutputProducer, "MISSING_OUTPUT_PRODUCER")           \
  X(kPortCardinalityMismatch, "PORT_CARDINALITY_MISMATCH")       \
  X(kPortProvenanceMismatch, "PORT_PROVENANCE_MISMATCH")         \
  X(kPortLifetimeMismatch, "PORT_LIFETIME_MISMATCH")             \
  X(kInternalException, "INTERNAL_EXCEPTION")                    \
  X(kModelMaterializationFailed, "MODEL_MATERIALIZATION_FAILED") \
  X(kNodeCreateFailed, "NODE_CREATE_FAILED")                     \
  X(kNodeInitFailed, "NODE_INIT_FAILED")                         \
  X(kInvalidBuildState, "INVALID_BUILD_STATE")

enum class DiagnosticCode {
#define LLM_EDGEFLOW_DEF_ENUM(name, str) name,
  LLM_EDGEFLOW_DIAGNOSTIC_CODES(LLM_EDGEFLOW_DEF_ENUM)
#undef LLM_EDGEFLOW_DEF_ENUM
};

const char* DiagnosticCodeName(DiagnosticCode code) noexcept;

}  // namespace llm_edgeflow

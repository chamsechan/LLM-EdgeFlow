#pragma once

// Framework-owned error codes. Zero is success; every failure has one meaning.
// Platform return values are translated only at the Operator boundary.
#define LLM_EDGEFLOW_ERROR_CODES(X)                                     \
  X(kOk, 0, "OK")                                                       \
  X(kInvalidArgument, -1001, "INVALID_ARGUMENT")                        \
  X(kInvalidInput, -1002, "INVALID_INPUT")                              \
  X(kInvalidOutput, -1003, "INVALID_OUTPUT")                            \
  X(kInvalidState, -1004, "INVALID_STATE")                              \
  X(kUnsupported, -1005, "UNSUPPORTED")                                 \
  X(kAllocationFailed, -1006, "ALLOCATION_FAILED")                      \
  X(kInferenceFailed, -1007, "INFERENCE_FAILED")                        \
  X(kOutputCountMismatch, -1008, "OUTPUT_COUNT_MISMATCH")               \
  X(kOutputProvenanceMismatch, -1009, "OUTPUT_PROVENANCE_MISMATCH")     \
  X(kInvalidUtf8, -1010, "INVALID_UTF8")                                \
  X(kPoolClosed, -1011, "POOL_CLOSED")                                  \
  X(kPoolCorrupt, -1012, "POOL_CORRUPT")                                \
  X(kPoolMissing, -1013, "POOL_MISSING")                                \
  X(kInvalidBatchPolicy, -1014, "INVALID_BATCH_POLICY")                 \
  X(kBusinessError, -1015, "BUSINESS_ERROR")                            \
  X(kMissingInput, -1016, "MISSING_INPUT")                              \
  X(kModelCallFailed, -1017, "MODEL_CALL_FAILED")                       \
  X(kRegexExecutionFailed, -1018, "REGEX_EXECUTION_FAILED")             \
  X(kRenderedOutputTooLong, -1019, "RENDERED_OUTPUT_TOO_LONG")          \
  X(kUnknownException, -1020, "UNKNOWN_EXCEPTION")                      \
  X(kUnsupportedControl, -1021, "UNSUPPORTED_CONTROL")                  \
  X(kJsonParse, -2002, "JSON_PARSE")                                    \
  X(kConfigFileOpen, -2003, "CONFIG_FILE_OPEN")                         \
  X(kRootType, -2004, "ROOT_TYPE")                                      \
  X(kUnknownField, -2005, "UNKNOWN_FIELD")                              \
  X(kMissingField, -2006, "MISSING_FIELD")                              \
  X(kFieldType, -2007, "FIELD_TYPE")                                    \
  X(kFieldRange, -2008, "FIELD_RANGE")                                  \
  X(kInvalidCombination, -2009, "INVALID_COMBINATION")                  \
  X(kDuplicateModelName, -2010, "DUPLICATE_MODEL_NAME")                 \
  X(kDuplicateNodeName, -2011, "DUPLICATE_NODE_NAME")                   \
  X(kInvalidNodeName, -2012, "INVALID_NODE_NAME")                       \
  X(kUnknownNodeReference, -2013, "UNKNOWN_NODE_REFERENCE")             \
  X(kUnknownPortReference, -2014, "UNKNOWN_PORT_REFERENCE")             \
  X(kPortTypeMismatch, -2015, "PORT_TYPE_MISMATCH")                     \
  X(kUnknownNodeType, -2016, "UNKNOWN_NODE_TYPE")                       \
  X(kUnknownModelType, -2017, "UNKNOWN_MODEL_TYPE")                     \
  X(kUnknownBackend, -2018, "UNKNOWN_BACKEND")                          \
  X(kModelTypeMismatch, -2019, "MODEL_TYPE_MISMATCH")                   \
  X(kBackendProtocolMismatch, -2020, "BACKEND_PROTOCOL_MISMATCH")       \
  X(kInvalidDependency, -2021, "INVALID_DEPENDENCY")                    \
  X(kDuplicateDependency, -2022, "DUPLICATE_DEPENDENCY")                \
  X(kDagCycle, -2023, "DAG_CYCLE")                                      \
  X(kRegistryConflict, -2024, "REGISTRY_CONFLICT")                      \
  X(kUnknownConfigField, -2025, "UNKNOWN_CONFIG_FIELD")                 \
  X(kMissingConfigField, -2026, "MISSING_CONFIG_FIELD")                 \
  X(kConfigFieldType, -2027, "CONFIG_FIELD_TYPE")                       \
  X(kConfigFieldRange, -2028, "CONFIG_FIELD_RANGE")                     \
  X(kConfigFieldEnum, -2029, "CONFIG_FIELD_ENUM")                       \
  X(kUnusedModel, -2030, "UNUSED_MODEL")                                \
  X(kUnknownModelReference, -2031, "UNKNOWN_MODEL_REFERENCE")           \
  X(kMissingInputProducer, -2032, "MISSING_INPUT_PRODUCER")             \
  X(kDuplicatePortProducer, -2033, "DUPLICATE_PORT_PRODUCER")           \
  X(kMissingOutputProducer, -2034, "MISSING_OUTPUT_PRODUCER")           \
  X(kPortCardinalityMismatch, -2035, "PORT_CARDINALITY_MISMATCH")       \
  X(kPortProvenanceMismatch, -2036, "PORT_PROVENANCE_MISMATCH")         \
  X(kPortLifetimeMismatch, -2037, "PORT_LIFETIME_MISMATCH")             \
  X(kInternalException, -2038, "INTERNAL_EXCEPTION")                    \
  X(kModelMaterializationFailed, -2039, "MODEL_MATERIALIZATION_FAILED") \
  X(kNodeCreateFailed, -2040, "NODE_CREATE_FAILED")                     \
  X(kNodeInitFailed, -2041, "NODE_INIT_FAILED")                         \
  X(kInvalidBuildState, -2042, "INVALID_BUILD_STATE")                   \
  X(kDeploymentError, -3064, "DEPLOYMENT_ERROR")                        \
  X(kDuplicateIoEntry, -3065, "DUPLICATE_IO_ENTRY")                     \
  X(kUnknownConverter, -3066, "UNKNOWN_CONVERTER")                      \
  X(kInvalidOutputAllocation, -3067, "INVALID_OUTPUT_ALLOCATION")       \
  X(kValidationFailed, -3068, "VALIDATION_FAILED")                      \
  X(kBodyTooLarge, -3069, "BODY_TOO_LARGE")                             \
  X(kDemoNotBuilt, -3070, "DEMO_NOT_BUILT")                             \
  X(kDeploymentMismatch, -3071, "DEPLOYMENT_MISMATCH")                  \
  X(kDeploymentNotAssociated, -3072, "DEPLOYMENT_NOT_ASSOCIATED")       \
  X(kDeploymentNotFound, -3073, "DEPLOYMENT_NOT_FOUND")                 \
  X(kDeploymentValidationFailed, -3074, "DEPLOYMENT_VALIDATION_FAILED") \
  X(kDocumentTooLarge, -3075, "DOCUMENT_TOO_LARGE")                     \
  X(kFileExists, -3076, "FILE_EXISTS")                                  \
  X(kInvalidAuthoringRequest, -3077, "INVALID_AUTHORING_REQUEST")       \
  X(kInvalidDeploymentConfig, -3078, "INVALID_DEPLOYMENT_CONFIG")       \
  X(kInvalidDeploymentName, -3079, "INVALID_DEPLOYMENT_NAME")           \
  X(kInvalidDeploymentPath, -3080, "INVALID_DEPLOYMENT_PATH")           \
  X(kInvalidInitRequest, -3081, "INVALID_INIT_REQUEST")                 \
  X(kInvalidPipelineDocument, -3083, "INVALID_PIPELINE_DOCUMENT")       \
  X(kInvalidPipelineFile, -3084, "INVALID_PIPELINE_FILE")               \
  X(kInvalidPipelineName, -3085, "INVALID_PIPELINE_NAME")               \
  X(kInvalidPipelinePath, -3086, "INVALID_PIPELINE_PATH")               \
  X(kInvalidProfile, -3087, "INVALID_PROFILE")                          \
  X(kNotFound, -3088, "NOT_FOUND")                                      \
  X(kPatchApplicationFailed, -3089, "PATCH_APPLICATION_FAILED")         \
  X(kPathEscape, -3090, "PATH_ESCAPE")                                  \
  X(kPipelineNotFound, -3091, "PIPELINE_NOT_FOUND")                     \
  X(kPipelineReadFailed, -3092, "PIPELINE_READ_FAILED")                 \
  X(kProfileMismatch, -3093, "PROFILE_MISMATCH")                        \
  X(kRevisionConflict, -3094, "REVISION_CONFLICT")                      \
  X(kRunBusy, -3095, "RUN_BUSY")                                        \
  X(kRunNotFound, -3096, "RUN_NOT_FOUND")                               \
  X(kRunTimeout, -3097, "RUN_TIMEOUT")                                  \
  X(kSaveFailed, -3098, "SAVE_FAILED")                                  \
  X(kSaveRollbackFailed, -3099, "SAVE_ROLLBACK_FAILED")                 \
  X(kSelectionCheckFailed, -3100, "SELECTION_CHECK_FAILED")             \
  X(kSymlinkRejected, -3101, "SYMLINK_REJECTED")                        \
  X(kToolNotBuilt, -3102, "TOOL_NOT_BUILT")                             \
  X(kToolOutdated, -3103, "TOOL_OUTDATED")                              \
  X(kToolProtocolError, -3104, "TOOL_PROTOCOL_ERROR")                   \
  X(kUnknownProfile, -3105, "UNKNOWN_PROFILE")                          \
  X(kInvalidFilePath, -4001, "INVALID_FILE_PATH")                       \
  X(kDeploymentConfig, -4002, "DEPLOYMENT_CONFIG")                      \
  X(kIoValidationError, -4003, "IO_VALIDATION_ERROR")                   \
  X(kJsonRead, -4004, "JSON_READ")                                      \
  X(kAuthoringError, -4005, "AUTHORING_ERROR")

namespace llm_edgeflow {
namespace error {
#define LLM_EDGEFLOW_ERROR_VALUE(name, value, text) \
  inline constexpr int name = value;
LLM_EDGEFLOW_ERROR_CODES(LLM_EDGEFLOW_ERROR_VALUE)
#undef LLM_EDGEFLOW_ERROR_VALUE
}  // namespace error

inline const char* ErrorCodeName(int code) noexcept {
  switch (code) {
#define LLM_EDGEFLOW_ERROR_NAME(name, value, text) \
  case error::name:                                \
    return text;
    LLM_EDGEFLOW_ERROR_CODES(LLM_EDGEFLOW_ERROR_NAME)
#undef LLM_EDGEFLOW_ERROR_NAME
  }
  return "UNRECOGNIZED_ERROR";
}

}  // namespace llm_edgeflow

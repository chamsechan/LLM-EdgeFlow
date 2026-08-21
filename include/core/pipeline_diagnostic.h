#pragma once

#include <string>

namespace alg_framework {

/**
 * @brief 结构化错误诊断码 (PipelineErrorCode)
 */
enum class PipelineErrorCode {
  kOk = 0,
  kJsonParse,
  kConfigFileOpen,
  kRootType,
  kUnknownField,
  kMissingField,
  kFieldType,
  kFieldRange,
  kInvalidCombination,
  kDuplicateModelId,
  kDuplicateNodeId,
  kUnknownNodeType,
  kUnknownEngineType,
  kInvalidDependency,
  kDagCycle,
  kRegistryConflict,
  kEngineCreateFailed,
  kEngineLoadFailed,
  kNodeCreateFailed,
  kNodeInitFailed,
  kInternalException,
  kInvalidBuildState,
};

/**
 * @brief 轻量级结构化诊断信息 (PipelineDiagnostic)
 */
struct PipelineDiagnostic {
  PipelineErrorCode code = PipelineErrorCode::kOk;
  std::string path;
  std::string message;

  bool IsOk() const { return code == PipelineErrorCode::kOk; }

  void Clear() {
    code = PipelineErrorCode::kOk;
    path.clear();
    message.clear();
  }
};

}  // namespace alg_framework

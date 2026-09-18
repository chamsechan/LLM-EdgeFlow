#pragma once

#include <nlohmann/json.hpp>
#include <optional>

#include "core/pipeline_validator.h"

namespace llm_edgeflow {

enum class DocumentValidationMode { kValidate, kExplain, kPlan };

struct DocumentValidationResult {
  bool ok = false;
  nlohmann::json response;
  std::optional<ValidationReport> core_report;
};

/**
 * @brief 校验 Pipeline 文档（CLI 与 Authoring 共用适配，RFC-0062）
 *
 * 统一处理带/不带 deployment 的文档，并根据模式调用
 * Validate/Explain/ValidateAndPlan， 投影有效模型路径来源，保留完整 Core 报告。
 */
DocumentValidationResult ValidatePipelineDocument(
    const nlohmann::json& document, DocumentValidationMode mode);

}  // namespace llm_edgeflow

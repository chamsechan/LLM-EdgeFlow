#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "core/pipeline_validator.h"

namespace llm_edgeflow {

enum class DocumentValidationMode { kValidate, kExplain, kPlan };

struct DocumentValidationResult {
  bool ok = false;
  nlohmann::json response;
  std::optional<ValidationReport> core_report;
};

/**
 * @brief 校验 Pipeline 文档（CLI 与 Authoring 共用适配）
 *
 * 通过共享接入准备入口解析 IO 边界，再调用 Core 校验并保留完整报告。
 */
DocumentValidationResult ValidatePipelineDocument(
    const nlohmann::json& document, DocumentValidationMode mode,
    const std::string& pipeline_dir = "");

}  // namespace llm_edgeflow

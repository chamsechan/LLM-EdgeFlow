#pragma once

#include <nlohmann/json.hpp>

#include "core/pipeline_validator.h"

namespace llm_edgeflow {

// 建立在 Validator 诊断之上的开发工具指引。规则仍只来自 PipelineValidator；
// 此处对诊断分类，补充事实和摘要，并给出 JSON Patch 修复建议，由 Validator
// 在打补丁后的文档上确认。不编译进 SDK。

// 为 report 中每条诊断补充修复原因、事实和摘要。
void AttachRemediation(const nlohmann::json& root, ValidationReport* report);

ValidationReport ValidateWithRemediation(
    const nlohmann::json& root,
    const PipelineIoBoundary* io_boundary = nullptr);

// 每条诊断另外最多保留三个已验证的修复。
ValidationReport ExplainPipeline(
    const nlohmann::json& root,
    const PipelineIoBoundary* io_boundary = nullptr);

}  // namespace llm_edgeflow

#pragma once

#include <functional>
#include <nlohmann/json.hpp>

#include "core/pipeline_validator.h"

namespace llm_edgeflow {

// 建立在 Validator 诊断之上的开发工具指引。规则仍只来自 PipelineValidator；
// 此处对诊断分类，补充事实和摘要，并给出 JSON Patch 修复建议，由 Validator
// 在打补丁后的文档上确认。不编译进 SDK。

// 为 report 中每条诊断补充修复原因、事实和摘要。
void AttachRemediation(const nlohmann::json& root, ValidationReport* report);

ValidationReport ValidateWithRemediation(const nlohmann::json& root,
                                         const PipelineIoBoundary& io_boundary);

// 每条诊断另外最多保留三个已验证的修复。
ValidationReport ExplainPipeline(const nlohmann::json& root,
                                 const PipelineIoBoundary& io_boundary);

// 文档入口在每次补丁后重新准备 I/O 和文件路径；中性测试入口复用同一修复流程。
ValidationReport ExplainPipeline(
    const nlohmann::json& document, ValidationReport report,
    const std::function<ValidationReport(const nlohmann::json&)>& revalidate);

}  // namespace llm_edgeflow

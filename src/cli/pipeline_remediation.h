#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "core/pipeline_validator.h"
#include "remediation_cause.h"

namespace llm_edgeflow {

// 建立在 Validator 诊断之上的开发工具指引。规则仍只来自 PipelineValidator；
// 此处对诊断分类，补充事实和摘要，并给出 JSON Patch 修复建议，由 Validator
// 在打补丁后的文档上确认。不编译进 SDK。

struct ValidationFix {
  std::string id;
  std::string title;
  std::string effect;
  nlohmann::json patch = nlohmann::json::array();
  std::string verification;  // "pipeline_valid" 或 "target_resolved"

  nlohmann::json ToJson() const;
};

struct ValidationRemediation {
  RemediationCause cause = RemediationCause::kUnknownConfigField;
  std::string summary;
  nlohmann::json facts = nlohmann::json::object();
  std::vector<ValidationFix> fixes;

  nlohmann::json ToJson() const;
};

// 工具诊断在中性诊断上附加修复信息；Core 不依赖这些类型。
struct ToolValidationDiagnostic : ValidationDiagnostic {
  std::optional<ValidationRemediation> remediation;

  nlohmann::json ToJson() const;
};

struct ToolValidationReport {
  explicit ToolValidationReport(ValidationReport report);

  bool ok = false;
  std::vector<ToolValidationDiagnostic> diagnostics;
  std::vector<std::string> topological_order;
  std::vector<std::vector<std::string>> topological_layers;

  nlohmann::json ToJson() const;
};

// 为 Core 报告补充原因、事实和摘要，保留中性报告的所有诊断和计划信息。
ToolValidationReport AttachRemediation(const nlohmann::json& root,
                                       ValidationReport report,
                                       const PipelineIoBoundary& io_boundary);

ToolValidationReport ValidateWithRemediation(
    const nlohmann::json& root, const PipelineIoBoundary& io_boundary);

// 每条诊断另外最多保留三个已验证的修复。
ToolValidationReport ExplainPipeline(const nlohmann::json& root,
                                     const PipelineIoBoundary& io_boundary);

// 文档入口在每次补丁后重新准备 I/O 和文件路径；重校验只消费 Core 报告。
ToolValidationReport ExplainPipeline(
    const nlohmann::json& document, ToolValidationReport report,
    const std::function<ValidationReport(const nlohmann::json&)>& revalidate);

}  // namespace llm_edgeflow

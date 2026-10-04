#pragma once

#include <nlohmann/json.hpp>

#include "core/pipeline_validator.h"

namespace llm_edgeflow {

// Developer-tool guidance layered on Validator diagnostics. Rules still come
// only from PipelineValidator; this code classifies its diagnostics, adds
// facts and a summary, and proposes JSON Patch fixes that the Validator
// confirms on the patched document. It is not compiled into the SDK.

// Adds remediation cause, facts and summary to each diagnostic of report.
void AttachRemediation(const nlohmann::json& root, ValidationReport* report);

ValidationReport ValidateWithRemediation(
    const nlohmann::json& root,
    const PipelineIoBoundary* io_boundary = nullptr);

// Also keeps up to three verified fixes per diagnostic.
ValidationReport ExplainPipeline(
    const nlohmann::json& root,
    const PipelineIoBoundary* io_boundary = nullptr);

}  // namespace llm_edgeflow

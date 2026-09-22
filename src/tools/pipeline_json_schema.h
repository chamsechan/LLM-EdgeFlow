#pragma once

#include <nlohmann/json.hpp>

namespace llm_edgeflow {

// Offline editor assistance for the capabilities of the selected tool build.
// Semantic validation remains in ValidatePipelineDocument / PipelineValidator.
nlohmann::json BuildPipelineJsonSchema(const nlohmann::json& catalog);

}  // namespace llm_edgeflow

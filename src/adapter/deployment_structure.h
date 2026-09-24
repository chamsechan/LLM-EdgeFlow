#pragma once

#include <nlohmann/json.hpp>

namespace llm_edgeflow {

// Owned by Integration. Allocator-specific params remain opaque JSON.
const nlohmann::json& PipelineDocumentStructure();
const nlohmann::json& DeploymentStructure();
const nlohmann::json& OutputAllocationStructure();

}  // namespace llm_edgeflow

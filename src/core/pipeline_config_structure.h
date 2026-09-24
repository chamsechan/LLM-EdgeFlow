#pragma once

#include <nlohmann/json.hpp>

namespace llm_edgeflow {

// Owned by Orchestration. Catalog-specific fields are added only by tooling.
const nlohmann::json& PipelineConfigStructure();

}  // namespace llm_edgeflow

#pragma once

#include <nlohmann/json.hpp>
#include <string_view>

namespace llm_edgeflow {

// Owned by Orchestration. Catalog-specific fields are added only by tooling.
const nlohmann::json& PipelineConfigStructure();
bool AllowsParallelWorkers(std::string_view execution_mode);

}  // namespace llm_edgeflow

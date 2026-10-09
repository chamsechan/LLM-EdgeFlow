#pragma once

#include <nlohmann/json.hpp>

namespace llm_edgeflow {
const nlohmann::json& PipelineDocumentStructure();
const nlohmann::json& IoStructure();
const nlohmann::json& IoEntryStructure(bool output = false);
}  // namespace llm_edgeflow

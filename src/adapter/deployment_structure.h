#pragma once

#include <nlohmann/json.hpp>

namespace llm_edgeflow {

// 归接入适配层所有。分配器专属参数保持为不透明 JSON。
const nlohmann::json& PipelineDocumentStructure();
const nlohmann::json& DeploymentStructure();
const nlohmann::json& OutputAllocationStructure();

}  // namespace llm_edgeflow

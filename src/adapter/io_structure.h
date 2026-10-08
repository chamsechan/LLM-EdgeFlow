#pragma once

#include <nlohmann/json.hpp>

namespace llm_edgeflow {

// 归接入适配层所有。converter 参数保持为不透明 JSON，由所选登记的参数声明校验。
const nlohmann::json& PipelineDocumentStructure();
const nlohmann::json& IoStructure();

}  // namespace llm_edgeflow

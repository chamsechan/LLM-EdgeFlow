#pragma once

#include <nlohmann/json.hpp>

namespace llm_edgeflow {

// 归流程编排层所有。Catalog 专属字段只由工具添加。
const nlohmann::json& PipelineConfigStructure();

}  // namespace llm_edgeflow

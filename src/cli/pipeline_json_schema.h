#pragma once

#include <nlohmann/json.hpp>

namespace llm_edgeflow {

// 为所选工具构建的能力提供离线编辑辅助。
// 语义校验仍由 ValidatePipelineDocument / PipelineValidator 负责。
nlohmann::json BuildPipelineJsonSchema();

}  // namespace llm_edgeflow

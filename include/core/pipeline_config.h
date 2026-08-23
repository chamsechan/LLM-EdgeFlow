#pragma once

#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/pipeline_diagnostic.h"

namespace alg_framework {

/**
 * @brief 解析后的单模型配置
 */
struct ParsedModelConfig {
  std::string model_id;
  std::string engine_type;
  std::string model_path;
  nlohmann::json config = nlohmann::json::object();
  size_t source_index = 0;
};

/**
 * @brief 解析后的单节点配置
 */
struct ParsedNodeConfig {
  std::string id;
  std::string node_type;
  std::vector<std::string> depends_on;
  nlohmann::json config = nlohmann::json::object();
  size_t source_index = 0;
};

/**
 * @brief 解析后的完整管线配置
 */
struct ParsedPipelineConfig {
  std::string business_name;
  std::string execution_mode = "sequential";
  size_t max_parallel_workers = 4;
  std::vector<ParsedModelConfig> models;
  std::vector<ParsedNodeConfig> nodes;
};

/**
 * @brief 严格解析与归一化 Pipeline JSON 配置
 * @param root 输入的原始 JSON 配置根节点
 * @param output 解析成功时填充的目标结构体
 * @param diagnostic 可选的错误诊断输出
 * @return true 解析成功，false 格式/类型/约束校验失败并填充 diagnostic
 */
bool ParsePipelineConfig(const nlohmann::json& root,
                         ParsedPipelineConfig* output,
                         PipelineDiagnostic* diagnostic = nullptr);

}  // namespace alg_framework

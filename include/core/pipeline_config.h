#pragma once

#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/pipeline_diagnostic.h"

namespace llm_edgeflow {

/**
 * @brief 解析后的单模型配置，独立声明 Model 与 Backend
 */
struct ParsedModelConfig {
  std::string model_id;
  size_t source_index = 0;

  // Model/Backend 配置字段
  std::string model_type;
  std::string backend;
  std::string model_path;
  nlohmann::json model_config = nlohmann::json::object();
  nlohmann::json backend_config = nlohmann::json::object();
};

/**
 * @brief 解析后的单节点端口映射配置
 */
struct ParsedPortBindings {
  std::unordered_map<std::string, std::string> inputs;
  std::unordered_map<std::string, std::string> outputs;
};

/**
 * @brief 解析后的单节点配置
 */
struct ParsedNodeConfig {
  std::string id;
  std::string node_type;
  std::vector<std::string> depends_on;
  ParsedPortBindings ports;
  nlohmann::json config = nlohmann::json::object();
  size_t source_index = 0;
};

/**
 * @brief 解析后的完整管线配置
 */
struct ParsedPipelineConfig {
  std::string biz_name;
  size_t max_parallel_workers = 1;
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

}  // namespace llm_edgeflow

#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

namespace llm_edgeflow {

/**
 * @brief 部署配置中的 I/O 接入定义 (RFC-0061)
 */
struct DeploymentIoSection {
  std::string io_binding;
  nlohmann::json output_allocations = nlohmann::json::object();
};

/**
 * @brief 部署配置定义 (RFC-0061: 包含模型路径覆盖与 I/O 接入配置)
 */
struct DeploymentSection {
  std::unordered_map<std::string, std::string> model_paths;
  bool has_model_paths = false;
  DeploymentIoSection io;
  bool has_io = false;
};

/**
 * @brief Pipeline 文档拆分结果 (RFC-0061)
 */
struct PipelineDocumentSplit {
  bool has_deployment = false;
  DeploymentSection deployment;
  nlohmann::json neutral_pipeline_json;
};

/**
 * @brief 拆分并严格校验 Pipeline JSON 中的 deployment 部分与中性 Pipeline 结构
 *
 * 严格校验 deployment:
 * - 根中若有 deployment，必须为对象，且只允许 model_paths 与 io 键
 * - model_paths 若存在必须为对象，键为 model_id，值为非空字符串路径
 * - io 必须存在且必须为对象，有且仅有 io_binding 和 output_allocations 两个字段
 * - neutral_pipeline_json 为移除已校验 deployment
 * 后的副本，保留所有其他根字段供 Core 校验
 */
bool SplitPipelineDocument(const nlohmann::json& root,
                           PipelineDocumentSplit* out_split,
                           std::string* out_error);

}  // namespace llm_edgeflow

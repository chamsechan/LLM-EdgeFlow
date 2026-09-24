#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "contracts/json_pointer.h"

namespace llm_edgeflow {

/**
 * @brief 部署配置中的 I/O 接入定义 (RFC-0061)
 */
struct DeploymentIoSection {
  std::string io_binding;
  nlohmann::json out_mem = nlohmann::json::object();
};

/**
 * @brief 部署配置定义：仅包含 I/O 接入配置
 */
struct DeploymentSection {
  DeploymentIoSection io;
};

/**
 * @brief Pipeline 文档拆分结果 (RFC-0061)
 */
struct PipelineDocumentSplit {
  DeploymentSection deployment;
  nlohmann::json neutral_pipeline_json;
};

/**
 * @brief 拆分并严格校验 Pipeline JSON 中的 deployment 部分与中性 Pipeline 结构
 *
 * 严格校验 deployment:
 * - 根中必须有 deployment；拒绝根级 biz_name
 * - deployment 必须为对象，且只允许 io 键
 * - io 必须为对象，含必需的 io_binding 和可选的 out_mem
 * - neutral_pipeline_json 为移除已校验 deployment
 * 后的副本，保留所有其他根字段供 Core 校验
 */
bool SplitPipelineDocument(const nlohmann::json& root,
                           PipelineDocumentSplit* out_split,
                           std::string* out_error,
                           std::string* out_error_path = nullptr);

}  // namespace llm_edgeflow

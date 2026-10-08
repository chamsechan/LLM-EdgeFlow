#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "adapter/deployment_io_config.h"
#include "adapter/deployment_preparation.h"
#include "adapter/io_converter.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "adapter/operator_io_contracts.h"
#include "core/pipeline_validator.h"

namespace llm_edgeflow {

/**
 * @brief 已验证的不可变接入计划 (同时包含 I/O 转换器选择与内部 Pipeline 计划)
 */
struct ValidatedIoPlan : IoSelection {
  // 外部文档快照：模型路径已解析，io 写入生效参数。
  nlohmann::json resolved_pipeline_json;

  std::unique_ptr<ValidatedPipelinePlan> pipeline_plan;
};

/**
 * @brief 接入计划解析器 (负责配置、转换器组合校验并调用 PipelineValidator
 * 进行中性边界验证)
 */
class IoPlanResolver {
 public:
  static int ResolveFromConfig(
      const DeploymentIoConfig& config, const std::string& model_root_dir,
      std::unique_ptr<ValidatedIoPlan>* out_plan, std::string* out_error,
      DeploymentDiagnostic* out_diagnostic = nullptr,
      uint32_t output_pool_depth = kDefaultOutputPoolDepth);

  static int ResolveFromFile(
      const std::string& config_path, const std::string& model_root_dir,
      std::unique_ptr<ValidatedIoPlan>* out_plan, std::string* out_error,
      DeploymentDiagnostic* out_diagnostic = nullptr,
      uint32_t output_pool_depth = kDefaultOutputPoolDepth);

  static int ResolveFromPipelineJson(
      const nlohmann::json& pipeline_json, const std::string& model_root_dir,
      std::unique_ptr<ValidatedIoPlan>* out_plan, std::string* out_error,
      DeploymentDiagnostic* out_diagnostic = nullptr,
      uint32_t output_pool_depth = kDefaultOutputPoolDepth);
};

}  // namespace llm_edgeflow

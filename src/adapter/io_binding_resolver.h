#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "adapter/deployment_io_config.h"
#include "adapter/io_binding.h"
#include "adapter/io_converter.h"
#include "adapter/operator_io_contracts.h"
#include "core/pipeline_validator.h"

namespace llm_edgeflow {

/**
 * @brief 已验证的不可变接入计划 (同时包含 I/O 转换器绑定与内部 Pipeline 计划)
 */
struct ValidatedIoPlan {
  IoBindingDefinition binding;
  const InputConverterDefinition* input_converter = nullptr;
  const OutputConverterDefinition* output_converter = nullptr;
  InputPortBindings input_port_bindings;
  OutputPortBindings output_port_bindings;
  size_t effective_max_batch_size = 64;

  std::unordered_map<std::string, ResolvedOutputPoolSpec> operator_output_specs;
  std::unordered_map<std::string, std::string> operator_output_parameter_texts;
  std::unordered_set<std::string> overridden_model_ids;
  nlohmann::json resolved_pipeline_json;

  std::unique_ptr<ValidatedPipelinePlan> pipeline_plan;
};

/**
 * @brief 接入绑定解析器 (负责配置、转换器组合校验并调用 PipelineValidator
 * 进行中性边界验证)
 */
class IoBindingResolver {
 public:
  static int ResolveFromConfig(const DeploymentIoConfig& config,
                               const std::string& transport,  // "operator"
                               const std::string& model_root_dir,
                               std::unique_ptr<ValidatedIoPlan>* out_plan,
                               std::string* out_error);

  static int ResolveFromFile(const std::string& config_path,
                             const std::string& transport,
                             const std::string& model_root_dir,
                             std::unique_ptr<ValidatedIoPlan>* out_plan,
                             std::string* out_error);

  static int ResolveFromPipelineJson(
      const nlohmann::json& pipeline_json,
      const std::string& transport,  // "operator"
      const std::string& model_root_dir,
      std::unique_ptr<ValidatedIoPlan>* out_plan, std::string* out_error);

  static int ResolveFromPipelineJson(
      const nlohmann::json& pipeline_json, const std::string& binding_id,
      const std::string& transport,  // "operator"
      const std::string& model_root_dir,
      std::unique_ptr<ValidatedIoPlan>* out_plan, std::string* out_error);
};

}  // namespace llm_edgeflow

#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapter/deployment_diagnostic.h"
#include "adapter/io_converter.h"
#include "adapter/operator_value_type.h"
#include "core/pipeline_validator.h"

namespace llm_edgeflow {
struct DeploymentPrepareOptions {
  std::string pipeline_dir;
};

struct SelectedInput {
  const InputConverterDefinition* converter = nullptr;
  IoPortBindings ports;
  std::shared_ptr<const ParameterValues> params;
  OperatorValueTypeBinding host_binding;
};

struct SelectedOutput {
  const OutputConverterDefinition* converter = nullptr;
  IoPortBindings ports;
  std::shared_ptr<const ParameterValues> params;
  ResolvedOutputPoolSpec pool_spec;
  OperatorValueTypeBinding host_binding;
  OperatorValueTypeBinding allocator_binding;
};

struct IoSelection {
  std::vector<SelectedInput> inputs;
  std::vector<SelectedOutput> outputs;
};

struct PreparedDeployment : IoSelection {
  nlohmann::json neutral_pipeline_json;
  ParsedPipelineConfig parsed_pipeline;
  PipelineIoBoundary io_boundary;
  void Clear() { *this = PreparedDeployment{}; }
};

/**
 * @brief 解析部署 I/O，绑定 Converter 与宿主类型，并准备容量参数及 Core
 * 的类型化边界。模型文件按 Pipeline
 * 目录解析；这里只准备配置，不加载模型或执行转换。
 * 成功后提交完整结果，失败时输出保持为空，并给出对应字段的诊断。
 */
bool PrepareDeploymentDocument(const nlohmann::json& document,
                               const DeploymentPrepareOptions& options,
                               PreparedDeployment* output,
                               DeploymentDiagnostic* diagnostic);
}  // namespace llm_edgeflow

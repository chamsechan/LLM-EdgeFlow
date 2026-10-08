#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapter/deployment_diagnostic.h"
#include "adapter/io_converter.h"
#include "adapter/operator_io_contracts.h"
#include "contracts/parameter_set.h"
#include "core/pipeline_validator.h"

namespace llm_edgeflow {

struct DeploymentPrepareOptions {
  // 为空时只做模型路径的格式检查；非空时模型路径解析到该目录内并检查
  // 文件留在根内。
  std::string model_root_dir;
};

// 从 io 解析出的一个输入项：登记及本句柄的参数。
struct SelectedInput {
  const InputConverterDefinition* converter = nullptr;
  std::shared_ptr<const ParameterValues> params;
};

// 一个输出项：登记、参数和由此生成的输出池规格。
struct SelectedOutput {
  const OutputConverterDefinition* converter = nullptr;
  std::shared_ptr<const ParameterValues> params;
  ResolvedOutputPoolSpec pool_spec;
};

// 与 io.input、io.output 顺序相同。由准备阶段生成，经校验的 Operator 计划
// 保持不变。
struct IoSelection {
  std::vector<SelectedInput> inputs;
  std::vector<SelectedOutput> outputs;
};

struct PreparedDeployment : IoSelection {
  nlohmann::json neutral_pipeline_json;
  PipelineIoBoundary io_boundary;

  void Clear() { *this = PreparedDeployment{}; }
};

/**
 * @brief 准备部署文档（共享接入准备入口）
 *
 * 拆出 io，按（type, name）选出每一项的登记，校验并补齐 converter 参数，
 * 生成输出池规格，解析模型路径，并由所选 converter 组成 IO 边界。
 */
bool PrepareDeploymentDocument(const nlohmann::json& document,
                               const DeploymentPrepareOptions& options,
                               PreparedDeployment* output,
                               DeploymentDiagnostic* diagnostic);

/**
 * @brief 生效的 io：每项写出 type、name 和（有参数时）全部生效参数值
 */
nlohmann::json EffectiveIoJson(const IoSelection& selection);

}  // namespace llm_edgeflow

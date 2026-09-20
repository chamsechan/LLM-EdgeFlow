#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "adapter/deployment_diagnostic.h"
#include "adapter/io_binding.h"
#include "adapter/io_converter.h"
#include "adapter/operator_io_contracts.h"
#include "core/pipeline_validator.h"

namespace llm_edgeflow {

enum class DeploymentPathMode {
  kLexicalOnly,
  kUnderRoot,
};

struct DeploymentPrepareOptions {
  DeploymentPathMode path_mode = DeploymentPathMode::kLexicalOnly;
  std::string model_root_dir;
};

struct PreparedDeployment {
  IoBindingDefinition binding;
  const InputConverterDefinition* input_converter = nullptr;
  const OutputConverterDefinition* output_converter = nullptr;
  InputPortBindings input_port_bindings;
  OutputPortBindings output_port_bindings;
  size_t effective_max_batch_size = 0;

  std::unordered_map<std::string, ResolvedOutputPoolSpec> output_specs;
  std::unordered_map<std::string, std::string> output_parameter_texts;
  std::unordered_set<std::string> overridden_model_ids;
  std::vector<std::string> model_path_source_pointers;  // 与 models 原顺序对应

  nlohmann::json neutral_pipeline_json;
  PipelineIoBoundary io_boundary;

  void Clear() {
    binding = IoBindingDefinition{};
    input_converter = nullptr;
    output_converter = nullptr;
    input_port_bindings = InputPortBindings{};
    output_port_bindings = OutputPortBindings{};
    effective_max_batch_size = 0;
    output_specs.clear();
    output_parameter_texts.clear();
    overridden_model_ids.clear();
    model_path_source_pointers.clear();
    neutral_pipeline_json = nullptr;
    io_boundary = PipelineIoBoundary{};
  }
};

/**
 * @brief 准备部署文档（S1–S7 共享接入准备入口，RFC-0062）
 */
bool PrepareDeploymentDocument(const nlohmann::json& document,
                               const DeploymentPrepareOptions& options,
                               PreparedDeployment* output,
                               DeploymentDiagnostic* diagnostic);

/**
 * @brief 将有效模型路径的 Core 诊断归位投影回原文档来源路径（RFC-0062）
 */
void ProjectModelPathDiagnostics(const PreparedDeployment& prepared,
                                 ValidationReport* report);

}  // namespace llm_edgeflow

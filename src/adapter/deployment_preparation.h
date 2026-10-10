#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapter/deployment_diagnostic.h"
#include "adapter/io_converter.h"
#include "core/pipeline_validator.h"

namespace llm_edgeflow {
struct DeploymentPrepareOptions {
  std::string pipeline_dir;
};

struct SelectedInput {
  const InputConverterDefinition* converter = nullptr;
  IoPortBindings ports;
  std::shared_ptr<const ParameterValues> params;
};

struct SelectedOutput {
  const OutputConverterDefinition* converter = nullptr;
  IoPortBindings ports;
  std::shared_ptr<const ParameterValues> params;
  ResolvedOutputPoolSpec pool_spec;
};

struct IoSelection {
  std::vector<SelectedInput> inputs;
  std::vector<SelectedOutput> outputs;
};

struct PreparedDeployment : IoSelection {
  nlohmann::json neutral_pipeline_json;
  PipelineIoBoundary io_boundary;
  void Clear() { *this = PreparedDeployment{}; }
};

bool PrepareDeploymentDocument(const nlohmann::json& document,
                               const DeploymentPrepareOptions& options,
                               PreparedDeployment* output,
                               DeploymentDiagnostic* diagnostic);
}  // namespace llm_edgeflow

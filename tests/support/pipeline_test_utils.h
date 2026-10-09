#pragma once

#include "core/pipeline.h"

namespace llm_edgeflow {
inline PipelineIoBoundary MakeTestBoundary(
    std::vector<IoPortDefinition> inputs = {},
    std::vector<IoPortDefinition> outputs = {}) {
  PipelineIoBoundary boundary;
  boundary.input_published_ports = std::move(inputs);
  boundary.output_consumed_ports = std::move(outputs);
  return boundary;
}

inline bool BuildTestPipeline(Pipeline& pipeline, const nlohmann::json& config,
                              const PipelineIoBoundary& boundary,
                              PipelineDiagnostic* diagnostic = nullptr) {
  return pipeline.BuildFromPlan(
      std::make_unique<ValidatedPipelinePlan>(
          PipelineValidator::ValidateAndPlan(config, boundary)),
      diagnostic);
}
}  // namespace llm_edgeflow

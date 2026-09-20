#pragma once

#include "core/pipeline.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {

inline void RegisterTestBizs(std::initializer_list<const char*> names,
                             std::vector<BizPortDefinition> ingress = {},
                             std::vector<BizPortDefinition> egress = {}) {
  for (const char* name : names) {
    if (!PipelineCatalog::FindBiz(name)) {
      PipelineCatalog::RegisterBizDefinition(
          {name, "test", "", ingress, egress});
    }
  }
}

inline bool BuildTestPipeline(Pipeline& pipeline, const nlohmann::json& config,
                              PipelineDiagnostic* diagnostic = nullptr) {
  return pipeline.BuildFromPlan(std::make_unique<ValidatedPipelinePlan>(
                                    PipelineValidator::ValidateAndPlan(config)),
                                diagnostic);
}

}  // namespace llm_edgeflow

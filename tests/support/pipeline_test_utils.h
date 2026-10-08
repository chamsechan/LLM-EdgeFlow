#pragma once

#include <utility>
#include <vector>

#include "adapter/biz_blackboard_keys.h"
#include "core/pipeline.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"

namespace llm_edgeflow {

// 直接构造 IO 边界：inputs 是输入项发布的端口，outputs 是输出项需要的端口。
inline PipelineIoBoundary MakeTestBoundary(
    std::vector<IoPortDefinition> inputs = {},
    std::vector<IoPortDefinition> outputs = {}) {
  PipelineIoBoundary boundary;
  boundary.input_published_ports = std::move(inputs);
  boundary.output_consumed_ports = std::move(outputs);
  return boundary;
}

// 关注词匹配的 IO 边界：输入项发布 input_sentences，输出项需要 rule_matches。
inline PipelineIoBoundary KeywordMatchTestBoundary() {
  return MakeTestBoundary(
      {IoPortDefinition{kInputSentences.name, kInputSentences.type_id, true}},
      {IoPortDefinition{kRuleMatches.name, kRuleMatches.type_id, true}});
}

// 没有任何输入项发布端口、没有输出项消费端口的边界。
inline const PipelineIoBoundary& EmptyTestBoundary() {
  static const PipelineIoBoundary boundary;
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

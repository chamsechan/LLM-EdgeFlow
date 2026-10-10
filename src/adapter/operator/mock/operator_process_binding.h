#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter/deployment_preparation.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "adapter/runtime_types.h"
#include "edgeflow/operator/interface.h"

namespace llm_edgeflow {

struct FrameOutputBinding {
  std::string key;
  size_t output_index = 0;
};

int ValidateAndExtractOperatorInputs(
    const llm_edgeflow::operator_api::NamedIoBatch& inputs,
    const std::vector<SelectedInput>& selected, const InputLimits& limits,
    std::vector<ExternalInputBatchView>* out_views, std::string* error);

int ResolveOperatorOutputs(
    const llm_edgeflow::operator_api::NamedIoBatch& outputs,
    const std::vector<SelectedOutput>& selected,
    std::vector<std::vector<FrameOutputBinding>>* frame_bindings,
    std::string* error);

void PublishOperatorOutputs(
    RuntimeOutputBatch* staged,
    const std::vector<std::vector<FrameOutputBinding>>& bindings,
    const std::vector<SelectedOutput>& selected,
    llm_edgeflow::operator_api::NamedIoBatch* outputs);

}  // namespace llm_edgeflow

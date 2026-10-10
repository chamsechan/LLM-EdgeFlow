#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter/deployment_preparation.h"
#include "adapter/operator/operator_output_pool.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "edgeflow/operator/interface.h"

namespace llm_edgeflow {

struct FrameOutputBinding {
  std::string key;
  size_t output_index = 0;
};

struct AcquiredOutputBlock {
  size_t frame_idx = 0;
  std::string key;
  std::shared_ptr<OutputPoolState> pool;
  void* raw_block = nullptr;
  size_t output_index = 0;
};

int ValidateAndExtractOperatorInputs(
    const llm_edgeflow::operator_api::NamedIoBatch& inputs,
    const std::vector<SelectedInput>& selected, const InputLimits& limits,
    std::vector<ExternalInputBatchView>* out_views,
    std::vector<uint64_t>* request_ids, std::string* error);

int ResolveOperatorOutputs(
    const llm_edgeflow::operator_api::NamedIoBatch& outputs,
    const std::vector<SelectedOutput>& selected,
    std::vector<std::vector<FrameOutputBinding>>* frame_bindings,
    std::string* error);

int AcquireOperatorOutputBlocks(
    const std::vector<std::vector<FrameOutputBinding>>& frame_bindings,
    const std::vector<std::shared_ptr<OutputPoolState>>& output_pools,
    ScopedOutputLeaseGuard* lease_guard,
    std::vector<AcquiredOutputBlock>* acquired_blocks, std::string* error);

void PublishOperatorOutputs(
    const std::vector<AcquiredOutputBlock>& acquired_blocks,
    llm_edgeflow::operator_api::NamedIoBatch* outputs,
    ScopedOutputLeaseGuard* lease_guard);

}  // namespace llm_edgeflow

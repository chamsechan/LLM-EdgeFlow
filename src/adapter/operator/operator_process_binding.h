#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter/deployment_preparation.h"
#include "adapter/io_converter.h"
#include "adapter/operator/operator_output_pool.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "edgeflow/operator/interface.h"

namespace llm_edgeflow {

struct FrameOutputBinding {
  std::string key;
  std::string logical_name;
  std::string type_suffix;
};

struct AcquiredOutputBlock {
  size_t frame_idx = 0;
  std::string key;
  std::shared_ptr<OutputPoolState> pool;
  void* raw_block = nullptr;
  std::string logical_name;
};

// 按每个输入项的 type 从宿主 key 中取出结构体，校验结构，并核对
// service_type 是否等于该项登记的取值。
int ValidateAndExtractOperatorInputs(
    const llm_edgeflow::operator_api::NamedIoBatch& inputs,
    const std::vector<SelectedInput>& items, const InputLimits& limits,
    ExternalInputBatchView* out_view, std::string* error);

int ResolveOperatorOutputs(
    const llm_edgeflow::operator_api::NamedIoBatch& outputs,
    const std::vector<SelectedOutput>& items,
    std::vector<std::vector<FrameOutputBinding>>* frame_bindings,
    std::string* error);

int AcquireOperatorOutputBlocks(
    const std::vector<std::vector<FrameOutputBinding>>& frame_bindings,
    const std::unordered_map<std::string, std::shared_ptr<OutputPoolState>>&
        output_pools,
    ScopedOutputLeaseGuard* lease_guard,
    std::vector<AcquiredOutputBlock>* acquired_blocks, std::string* error);

void PublishOperatorOutputs(
    const std::vector<AcquiredOutputBlock>& acquired_blocks,
    llm_edgeflow::operator_api::NamedIoBatch* outputs,
    ScopedOutputLeaseGuard* lease_guard);

}  // namespace llm_edgeflow

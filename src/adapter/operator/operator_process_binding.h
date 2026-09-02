#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter/operator/operator_biz_bridge_registry.h"
#include "adapter/operator/operator_output_pool.h"
#include "operator/operator_interface.h"

namespace llm_edgeflow {

struct FrameOutputBinding {
  std::string key;
  std::string canonical_suffix;
};

struct AcquiredOutputBlock {
  size_t frame_idx = 0;
  std::string key;
  std::shared_ptr<OutputPoolState> pool;
  void* raw_block = nullptr;
};

int ConvertOperatorInputs(
    const llm_edgeflow::operator_api::NamedIoBatch& inputs,
    const OperatorBizBridgeDescriptor& bridge,
    const ResolvedInputLimits& limits,
    ProcessLocalShadowStorage* shadow_storage,
    std::vector<const void*>* internal_dtos, std::string* error);

int ResolveOperatorOutputs(
    const llm_edgeflow::operator_api::NamedIoBatch& outputs,
    const OperatorBizBridgeDescriptor& bridge,
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

#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "contracts/traceable_item.h"

namespace llm_edgeflow {

enum class TraceableAlignmentError {
  kNone,
  kCountMismatch,
  kProvenanceMismatch,
};

struct TraceableAlignmentResult {
  TraceableAlignmentError error = TraceableAlignmentError::kNone;
  size_t mismatch_index = 0;

  bool IsAligned() const noexcept {
    return error == TraceableAlignmentError::kNone;
  }
};

/**
 * @brief 校验严格 1:1、保序的 Traceable 批次契约。
 *
 * 数量不一致时，mismatch_index 是任一批次中首个缺失的索引；
 * 完全对齐时，mismatch_index 等于批大小。
 */
template <typename Input, typename Output>
[[nodiscard]] TraceableAlignmentResult ValidatePreservedTraceableAlignment(
    const std::vector<TraceableItem<Input>>& inputs,
    const std::vector<TraceableItem<Output>>& outputs) noexcept {
  if (inputs.size() != outputs.size()) {
    return {TraceableAlignmentError::kCountMismatch,
            std::min(inputs.size(), outputs.size())};
  }

  for (size_t i = 0; i < inputs.size(); ++i) {
    if (inputs[i].req_id != outputs[i].req_id ||
        inputs[i].sub_id != outputs[i].sub_id) {
      return {TraceableAlignmentError::kProvenanceMismatch, i};
    }
  }
  return {TraceableAlignmentError::kNone, inputs.size()};
}

}  // namespace llm_edgeflow

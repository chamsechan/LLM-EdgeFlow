#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "contracts/traceable_item.h"

namespace alg_framework {

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
 * @brief Validate a strict 1:1, order-preserving Traceable batch contract.
 *
 * For a count mismatch, mismatch_index is the first index missing from either
 * batch. For an aligned result, mismatch_index equals the batch size.
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

}  // namespace alg_framework

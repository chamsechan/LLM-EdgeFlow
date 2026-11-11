#pragma once

#include <utility>
#include <vector>

#include "contracts/traceable_item.h"
#include "nodes/node_result.h"

namespace llm_edgeflow {

/**
 * @brief 逐条转换 payload，并保留输入顺序及每条数据的来源编号。
 * 普通回调使本函数返回批次；回调返回 NodeResult 时，本函数也返回 NodeResult，
 * 任一条目失败即终止，并在缺少批次诊断时补上失败条目的来源键。
 */
template <typename InT, typename MapFn>
auto MapPayloads(const std::vector<TraceableItem<InT>>& input, MapFn&& fn) {
  using RawResult = decltype(fn(std::declval<const InT&>()));
  if constexpr (IsNodeResult<RawResult>::value) {
    using OutPayload = typename IsNodeResult<RawResult>::ValueType;
    using OutBatch = std::vector<TraceableItem<OutPayload>>;
    OutBatch outputs;
    outputs.reserve(input.size());
    for (const auto& item : input) {
      auto res = fn(item.data);
      if (!res.ok()) {
        // 诊断指出失败的条目；回调已给出的结构化细节优先。
        auto failure = std::move(res).ExtractFailure();
        if (!failure.batch_detail.has_value()) {
          failure.batch_detail = BatchFailureDetail{
              "MapPayloads", BatchFailureReason::kCallbackFailed,
              TraceableItemKey{item.req_id, item.sub_id}};
        }
        return NodeResult<OutBatch>::Failure(std::move(failure));
      }
      outputs.emplace_back(item.req_id, item.sub_id, std::move(res).value());
    }
    return NodeResult<OutBatch>::Success(std::move(outputs));
  } else {
    using OutPayload = RawResult;
    using OutBatch = std::vector<TraceableItem<OutPayload>>;
    OutBatch outputs;
    outputs.reserve(input.size());
    for (const auto& item : input) {
      outputs.emplace_back(item.req_id, item.sub_id, fn(item.data));
    }
    return outputs;
  }
}

}  // namespace llm_edgeflow

#pragma once

#include <utility>
#include <vector>

#include "contracts/traceable_item.h"
#include "nodes/node_result.h"

namespace llm_edgeflow {

namespace detail {

template <typename T>
struct IsNodeResultType : std::false_type {
  using ValueType = T;
};

template <typename T>
struct IsNodeResultType<NodeResult<T>> : std::true_type {
  using ValueType = T;
};

}  // namespace detail

template <typename InT, typename MapFn>
auto MapPayloads(const std::vector<TraceableItem<InT>>& input, MapFn&& fn) {
  using RawResult = decltype(fn(std::declval<const InT&>()));
  if constexpr (detail::IsNodeResultType<RawResult>::value) {
    using OutPayload = typename detail::IsNodeResultType<RawResult>::ValueType;
    using OutBatch = std::vector<TraceableItem<OutPayload>>;
    OutBatch outputs;
    outputs.reserve(input.size());
    for (const auto& item : input) {
      auto res = fn(item.data);
      if (!res.ok()) {
        return NodeResult<OutBatch>::Failure(std::move(res).ExtractFailure());
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

#pragma once

#include <string>
#include <vector>

#include "business/cross_rerank/cross_rerank_dto.h"
#include "core/blackboard_key.h"
#include "core/common_contracts.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"

namespace alg_framework {

inline constexpr BlackboardKey<std::vector<RerankQueryInput>> kRawRerankInputs{
    "raw_rerank_inputs", "vector<RerankQueryInput>"};

inline constexpr BlackboardKey<
    std::vector<TraceableItem<IRerankEngine::PairInput>>>
    kRerankPairItems{"rerank_pair_items", "traceable<PairInput>[]"};

inline constexpr BlackboardKey<std::vector<int>> kRerankCountsPerReq{
    "rerank_counts_per_req", "vector<int>"};

inline constexpr BlackboardKey<std::vector<TraceableItem<float>>>
    kRerankScoredItems{"rerank_scored_items", "traceable<float>[]"};

inline constexpr BlackboardKey<std::vector<RerankQueryResult>>
    kRerankBatchFinalOutputs{"rerank_batch_final_outputs",
                             "vector<RerankQueryResult>"};

}  // namespace alg_framework

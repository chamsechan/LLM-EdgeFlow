#include <algorithm>
#include <iostream>
#include <numeric>
#include <vector>

#include "business/cross_rerank/cross_rerank_contract.h"
#include "business/cross_rerank/cross_rerank_dto.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "nodes/node_support.h"

namespace alg_framework {

class RerankSortPostNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "RerankSortPostNode";

  RerankSortPostNode() : NodeBase(kNodeType) {}

 protected:
  int ProcessNode(AlgContext& req_ctx) override {
    const auto* raw_inputs = Require(req_ctx, kRawRerankInputs, -7301);
    const auto* scored_items = Require(req_ctx, kRerankScoredItems, -7301);

    if (!raw_inputs || !scored_items) {
      return -7301;
    }

    std::vector<RerankQueryResult> outputs(raw_inputs->size());
    size_t score_idx = 0;

    for (size_t i = 0; i < raw_inputs->size(); ++i) {
      const auto& in_req = (*raw_inputs)[i];
      outputs[i].request_id = in_req.request_id;
      int cand_count = static_cast<int>(in_req.candidate_passages.size());
      outputs[i].count = cand_count;
      outputs[i].status_code = 0;

      std::vector<std::pair<float, int>> score_with_orig_idx;
      for (int c = 0; c < cand_count; ++c) {
        if (score_idx < scored_items->size()) {
          score_with_orig_idx.push_back({(*scored_items)[score_idx].data, c});
          score_idx++;
        }
      }

      // 按打分降序排列
      std::sort(score_with_orig_idx.begin(), score_with_orig_idx.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });

      outputs[i].scores.resize(score_with_orig_idx.size());
      outputs[i].sorted_indices.resize(score_with_orig_idx.size());
      for (size_t k = 0; k < score_with_orig_idx.size(); ++k) {
        outputs[i].scores[k] = score_with_orig_idx[k].first;
        outputs[i].sorted_indices[k] = score_with_orig_idx[k].second;
      }
    }

    Publish(req_ctx, kRerankBatchFinalOutputs, std::move(outputs));
    return 0;
  }
};

NodeDefinition MakeRerankSortPostNodeDefinition() {
  NodeDefinition def;
  def.node_type = RerankSortPostNode::kNodeType;
  def.category = "business";
  def.description = "Cross rerank scoring sort and post-processing node";
  def.inputs = {RequiredInput(kRawRerankInputs),
                RequiredInput(kRerankScoredItems)};
  def.outputs = {Output(kRerankBatchFinalOutputs)};
  def.business_names = {kCrossRerankBusinessName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(RerankSortPostNode,
                              MakeRerankSortPostNodeDefinition());

}  // namespace alg_framework

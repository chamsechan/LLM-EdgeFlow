#include <iostream>
#include <string>
#include <vector>

#include "business/cross_rerank/cross_rerank_contract.h"
#include "business/cross_rerank/cross_rerank_dto.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"
#include "nodes/node_support.h"

namespace alg_framework {

class RerankPairBuilderNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "RerankPairBuilderNode";

  RerankPairBuilderNode() : NodeBase(kNodeType) {}

 protected:
  int ProcessNode(AlgContext& req_ctx) override {
    const auto* raw_inputs = Require(req_ctx, kRawRerankInputs, -7101);
    if (!raw_inputs) {
      return -7101;
    }

    std::vector<TraceableItem<IRerankEngine::PairInput>> pair_items;
    std::vector<int> counts_per_req;

    for (size_t i = 0; i < raw_inputs->size(); ++i) {
      const auto& item = (*raw_inputs)[i];
      int cand_count = static_cast<int>(item.candidate_passages.size());
      counts_per_req.push_back(cand_count);
      for (int c = 0; c < cand_count; ++c) {
        IRerankEngine::PairInput pair;
        pair.query = item.query_text;
        pair.passage = item.candidate_passages[c];
        pair_items.emplace_back(item.request_id, c, std::move(pair));
      }
    }

    Publish(req_ctx, kRerankPairItems, std::move(pair_items));
    Publish(req_ctx, kRerankCountsPerReq, std::move(counts_per_req));
    return 0;
  }
};

NodeDefinition MakeRerankPairBuilderNodeDefinition() {
  NodeDefinition def;
  def.node_type = RerankPairBuilderNode::kNodeType;
  def.category = "business";
  def.description = "Cross rerank pair builder node";
  def.inputs = {RequiredInput(kRawRerankInputs)};
  def.outputs = {Output(kRerankPairItems), Output(kRerankCountsPerReq)};
  def.business_names = {kCrossRerankBusinessName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(RerankPairBuilderNode,
                              MakeRerankPairBuilderNodeDefinition());

}  // namespace alg_framework

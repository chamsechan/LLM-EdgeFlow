#include <iostream>
#include <string>
#include <vector>

#include "business/cross_rerank/cross_rerank_dto.h"
#include "core/alg_context.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"

namespace alg_framework {

class RerankPairBuilderNode : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    (void)config;
    (void)session_ctx;
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* raw_inputs =
        req_ctx->Get<std::vector<RerankQueryInput>>("raw_rerank_inputs");
    if (!raw_inputs) {
      req_ctx->SetError(-7101,
                        "RerankPairBuilderNode: Missing raw_rerank_inputs");
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

    req_ctx->Set("rerank_pair_items", std::move(pair_items));
    req_ctx->Set("rerank_counts_per_req", std::move(counts_per_req));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "RerankPairBuilderNode";
    return name;
  }
};

REGISTER_NODE(RerankPairBuilderNode);

}  // namespace alg_framework

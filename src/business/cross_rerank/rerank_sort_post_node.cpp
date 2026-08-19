#include <algorithm>
#include <iostream>
#include <numeric>
#include <vector>

#include "business/cross_rerank/cross_rerank_dto.h"
#include "core/alg_context.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"

namespace alg_framework {

class RerankSortPostNode : public INode {
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
    auto* scored_items =
        req_ctx->Get<std::vector<TraceableItem<float>>>("rerank_scored_items");

    if (!raw_inputs || !scored_items) {
      req_ctx->SetError(-7301, "RerankSortPostNode: Missing inputs or scores");
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

    req_ctx->Set("rerank_batch_final_outputs", std::move(outputs));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "RerankSortPostNode";
    return name;
  }
};

REGISTER_NODE(RerankSortPostNode);

}  // namespace alg_framework

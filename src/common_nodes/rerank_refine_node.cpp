#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"

namespace alg_framework {

/**
 * @brief 通用 Cross-Encoder 精排重排序与精选算子 (RAG
 * 粗筛向量检索后的高精度重排过滤)
 */
class RerankRefineNode : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    bind_model_ = config.value("bind_model", "rerank_model_v1");
    top_k_ = config.value("top_k", 1);
    candidates_key_ = config.value("candidates_key", "matched_top_chunks");
    query_key_ = config.value("query_key", "raw_queries");
    output_key_ = config.value("output_key", "matched_top_chunks");

    rerank_engine_ =
        session_ctx->GetModelManager().GetModel<IRerankEngine>(bind_model_);
    if (!rerank_engine_) {
      std::cerr << "[RerankRefineNode] Failed to bind model: " << bind_model_
                << std::endl;
      return false;
    }
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* candidates =
        req_ctx->Get<std::vector<TraceableItem<std::string>>>(candidates_key_);
    auto* queries = req_ctx->Get<std::vector<std::string>>(query_key_);
    if (!candidates || !queries) {
      req_ctx->SetError(
          -3201, "RerankRefineNode: Missing candidates or queries in context");
      return -3201;
    }

    if (candidates->empty()) {
      return 0;
    }

    // 构造带溯源的 (Query, Chunk) 样本对
    std::vector<TraceableItem<IRerankEngine::PairInput>> pair_items;
    pair_items.reserve(candidates->size());
    for (const auto& item : *candidates) {
      std::string query =
          (item.req_id < queries->size()) ? (*queries)[item.req_id] : "";
      pair_items.emplace_back(
          item.req_id, item.sub_id,
          IRerankEngine::PairInput{std::move(query), item.data});
    }

    std::vector<TraceableItem<float>> pair_scores;
    std::cout << "[RerankRefineNode] Scoring " << pair_items.size()
              << " candidate (query, chunk) pairs with Reranker..."
              << std::endl;
    int ret = rerank_engine_->ScoreTraceableBatch(pair_items, &pair_scores);
    if (ret != 0) {
      req_ctx->SetError(ret,
                        "RerankRefineNode: Rerank engine inference failed");
      return ret;
    }

    // 按 req_id 分组并按打分降序排列
    struct ScoredCandidate {
      TraceableItem<std::string> item;
      float score;
    };
    std::map<uint32_t, std::vector<ScoredCandidate>> req_scored_map;
    for (size_t i = 0; i < pair_scores.size() && i < candidates->size(); ++i) {
      req_scored_map[(*candidates)[i].req_id].push_back(
          {(*candidates)[i], pair_scores[i].data});
    }

    std::vector<TraceableItem<std::string>> refined_top_chunks;
    for (auto& [r_id, list] : req_scored_map) {
      std::sort(list.begin(), list.end(),
                [](const ScoredCandidate& a, const ScoredCandidate& b) {
                  return a.score > b.score;
                });
      for (size_t k = 0; k < std::min(top_k_, list.size()); ++k) {
        std::cout << "  [Rerank Top-" << k << "] Req #" << r_id
                  << " (Score: " << list[k].score << ") -> \""
                  << list[k].item.data.substr(0, 40) << "...\"" << std::endl;
        refined_top_chunks.push_back(list[k].item);
      }
    }

    req_ctx->Set(output_key_, std::move(refined_top_chunks));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "RerankRefineNode";
    return name;
  }

 private:
  std::string bind_model_;
  size_t top_k_{1};
  std::string candidates_key_;
  std::string query_key_;
  std::string output_key_;
  std::shared_ptr<IRerankEngine> rerank_engine_;
};

REGISTER_NODE(RerankRefineNode);

}  // namespace alg_framework

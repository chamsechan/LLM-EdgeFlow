#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "biz/doc_qa/doc_qa_contract.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"
#include "nodes/model_bound_node.h"

namespace alg_framework {

/**
 * @brief Cross-Encoder 精排重排序与精选算子 (RAG
 * 粗筛向量检索后的高精度重排过滤)
 */
class RerankRefineNode final : public ModelBoundNode<IRerankEngine> {
 public:
  inline static constexpr char kNodeType[] = "RerankRefineNode";

  RerankRefineNode()
      : ModelBoundNode<IRerankEngine>(kNodeType, "rerank_model_v1") {}

 protected:
  bool InitModelNode(const nlohmann::json& config,
                     SessionContext& /*session_ctx*/) override {
    top_k_ = config.value("top_k", 1);
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* candidates = Require(req_ctx, kMatchedTopChunks, -3201);
    const auto* queries = Require(req_ctx, kRawQueries, -3201);
    if (!candidates || !queries) {
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
    int ret = engine()->ScoreTraceableBatch(pair_items, &pair_scores);
    if (ret != 0) {
      return Fail(req_ctx, ret,
                  "RerankRefineNode: Rerank engine inference failed");
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

    Publish(req_ctx, kMatchedTopChunks, std::move(refined_top_chunks));
    return 0;
  }

 private:
  size_t top_k_ = 1;
};

NodeDefinition MakeRerankRefineNodeDefinition() {
  NodeDefinition def;
  def.node_type = RerankRefineNode::kNodeType;
  def.category = "biz";
  def.description = "Cross-encoder rerank refine ranking node";
  def.inputs = {RequiredInput(kMatchedTopChunks), RequiredInput(kRawQueries)};
  def.outputs = {Output(kMatchedTopChunks, /*allow_override=*/true)};
  def.config_fields = {
      ConfigFieldDefinition{"bind_model", ConfigValueKind::kString, false,
                            "rerank_model_v1"},
      ConfigFieldDefinition{"top_k", ConfigValueKind::kInteger, false, 1, 1.0,
                            100.0}};
  def.model_capability = "rerank";
  def.model_config_field = "bind_model";
  def.biz_names = {kDocQaRerankBizName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(RerankRefineNode,
                              MakeRerankRefineNodeDefinition());

}  // namespace alg_framework

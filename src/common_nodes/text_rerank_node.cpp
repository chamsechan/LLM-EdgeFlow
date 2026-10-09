#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace {
struct Inputs {
  const TextBatch* queries = nullptr;
  const RankedTextBatch* candidates = nullptr;
};
struct Params {
  int top_k{};
};
struct Models {
  RerankCall reranker;
};

NodeResult<RankedTextBatch> Run(const Inputs& inputs, const Params& params,
                                const Models& models) {
  const auto* queries = inputs.queries;
  const auto* candidates = inputs.candidates;

  QueryCandidatesBatch pair_items;
  struct CandidatePayload {
    uint32_t req_id;
    uint32_t sub_id;
    std::string text;
  };
  std::vector<CandidatePayload> cand_payloads;

  std::unordered_map<uint32_t, std::string> query_map;
  for (const auto& q : *queries) {
    if (!query_map.emplace(q.req_id, q.data).second) {
      return NodeResult<RankedTextBatch>::Failure(
          NodeErrorKind::kBusinessError,
          "text_rerank requires one query per request; duplicate req_id=" +
              std::to_string(q.req_id));
    }
  }

  pair_items.reserve(candidates->size());
  cand_payloads.reserve(candidates->size());
  for (const auto& c : *candidates) {
    const auto query = query_map.find(c.req_id);
    if (query == query_map.end()) {
      return NodeResult<RankedTextBatch>::Failure(
          NodeErrorKind::kBusinessError,
          "Candidate has no query for req_id=" + std::to_string(c.req_id));
    }
    std::string q = query->second;
    pair_items.emplace_back(c.req_id, c.sub_id,
                            QueryCandidatePair{std::move(q), c.data.text});
    cand_payloads.push_back({c.req_id, c.sub_id, c.data.text});
  }

  auto scores = models.reranker.Score(pair_items);
  if (!scores.ok())
    return NodeResult<RankedTextBatch>::Failure(scores.failure());
  auto pair_scores = std::move(scores).value();

  // 按 req_id 分组排序
  struct ScoredCandidate {
    uint32_t original_sub_id;
    std::string text;
    float score;
  };
  std::map<uint32_t, std::vector<ScoredCandidate>> req_scored_map;
  for (size_t i = 0; i < pair_scores.size() && i < cand_payloads.size(); ++i) {
    req_scored_map[cand_payloads[i].req_id].push_back(
        {cand_payloads[i].sub_id, std::move(cand_payloads[i].text),
         pair_scores[i].data});
  }

  RankedTextBatch refined_batch;
  for (auto& [r_id, list] : req_scored_map) {
    std::sort(list.begin(), list.end(),
              [](const ScoredCandidate& a, const ScoredCandidate& b) {
                return a.score > b.score;
              });
    size_t count = std::min(static_cast<size_t>(params.top_k), list.size());
    for (size_t k = 0; k < count; ++k) {
      RankedCandidate rc(std::move(list[k].text), list[k].score,
                         static_cast<int>(k + 1), list[k].original_sub_id);
      refined_batch.emplace_back(r_id, static_cast<uint32_t>(k), std::move(rc));
    }
  }

  return NodeResult<RankedTextBatch>::Success(std::move(refined_batch));
}

auto Spec() {
  return MakeNodeSpec(
             InputsOf<Inputs>(
                 {Required("queries", &Inputs::queries),
                  Required("candidates", &Inputs::candidates,
                           PortFlow{"N:1", "preserve", "request"})}),
             ProducedBatch<RankedTextBatch>(
                 "ranked", PortFlow{"1:N", "generate_sub_id", "request"}),
             Parameters<Params>(
                 {Field("top_k", &Params::top_k)
                      .Default(1)
                      .Range(1, 1000)
                      .Description(
                          "按 req_id "
                          "分组，用重排模型分数降序保留的候选条数上限。")}),
             ModelsOf<Models>(
                 {Model("reranker", "bind_model", &Models::reranker)}),
             &Run)
      .Category("common")
      .Description("Cross-encoder semantic reranking and top-k node")
      .ParallelSafe(true);
}
}  // namespace
REGISTER_FUNCTION_NODE(text_rerank, Spec());
}  // namespace llm_edgeflow

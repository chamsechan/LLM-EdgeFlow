#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nodes/authoring.h"
#include "nodes/node_error_codes.h"

namespace llm_edgeflow {
namespace {
struct RerankInputs {
  const TextBatch* queries = nullptr;
  const RankedTextBatch* candidates = nullptr;
  const TextBatch* candidate_texts = nullptr;
  const QueryCandidatesBatch* pairs = nullptr;
};
struct RerankParams {
  int top_k{};
};
struct RerankModels {
  RerankCall reranker;
};

NodeResult<RankedTextBatch> RerankText(const RerankInputs& inputs,
                                       const RerankParams& params,
                                       const RerankModels& models) {
  const auto* queries = inputs.queries;
  const auto* candidates = inputs.candidates;
  const auto* candidate_texts = inputs.candidate_texts;
  const auto* pairs = inputs.pairs;
  if (!pairs && !queries) {
    return NodeResult<RankedTextBatch>::Failure(
        NodeErrorKind::kBusinessError,
        "TextRerankNode requires pairs or queries input",
        node_error::text_rerank::kMissingInput);
  }

  QueryCandidatesBatch pair_items;
  struct CandidatePayload {
    uint32_t req_id;
    uint32_t sub_id;
    std::string text;
  };
  std::vector<CandidatePayload> cand_payloads;

  if (pairs && !pairs->empty()) {
    pair_items.reserve(pairs->size());
    cand_payloads.reserve(pairs->size());
    for (const auto& item : *pairs) {
      pair_items.emplace_back(
          item.req_id, item.sub_id,
          QueryCandidatePair{item.data.query, item.data.candidate});
      cand_payloads.push_back({item.req_id, item.sub_id, item.data.candidate});
    }
  } else if (queries && !queries->empty()) {
    std::unordered_map<uint32_t, std::string> query_map;
    for (const auto& q : *queries) {
      query_map[q.req_id] = q.data;
    }

    if (candidates && !candidates->empty()) {
      pair_items.reserve(candidates->size());
      cand_payloads.reserve(candidates->size());
      for (const auto& c : *candidates) {
        std::string q = (query_map.find(c.req_id) != query_map.end())
                            ? query_map[c.req_id]
                            : "";
        pair_items.emplace_back(c.req_id, c.sub_id,
                                QueryCandidatePair{std::move(q), c.data.text});
        cand_payloads.push_back({c.req_id, c.sub_id, c.data.text});
      }
    } else if (candidate_texts && !candidate_texts->empty()) {
      pair_items.reserve(candidate_texts->size());
      cand_payloads.reserve(candidate_texts->size());
      for (const auto& c : *candidate_texts) {
        std::string q = (query_map.find(c.req_id) != query_map.end())
                            ? query_map[c.req_id]
                            : "";
        pair_items.emplace_back(c.req_id, c.sub_id,
                                QueryCandidatePair{std::move(q), c.data});
        cand_payloads.push_back({c.req_id, c.sub_id, c.data});
      }
    }
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

auto TextRerankSpec() {
  return MakeBatchSpec(
             InputsOf<RerankInputs>(
                 {OptionalValue("queries", &RerankInputs::queries),
                  OptionalValue("candidates", &RerankInputs::candidates,
                                PortFlow{"N:1", "preserve", "request"}),
                  OptionalValue("candidate_texts",
                                &RerankInputs::candidate_texts,
                                PortFlow{"N:1", "preserve", "request"}),
                  OptionalValue("pairs", &RerankInputs::pairs)}),
             ProducedBatch<RankedTextBatch>(
                 "ranked", PortFlow{"1:N", "generate_sub_id", "request"}),
             Parameters<RerankParams>(
                 {Field("top_k", &RerankParams::top_k)
                      .Default(1)
                      .Range(1, 1000)
                      .Description(
                          "按 req_id "
                          "分组，用重排模型分数降序保留的候选条数上限。")}),
             ModelsOf<RerankModels>(
                 {Model("reranker", "bind_model", &RerankModels::reranker,
                        "rerank_model_v1",
                        "引用 models[].model_id；所选模型必须提供 rerank "
                        "查询与候选评分能力。")}),
             &RerankText)
      .PortConstraints({PortGroupConstraint::Groups(
          PortConstraintKind::kExactOneGroupOf,
          {{"pairs"},
           {"queries", "candidates"},
           {"queries", "candidate_texts"}},
          "TextRerankNode requires exactly one input group: [pairs], [queries, "
          "candidates], or [queries, candidate_texts]")})
      .Category("common")
      .Description("Cross-encoder semantic reranking and top-k node")
      .ParallelSafe(true);
}
}  // namespace
REGISTER_FUNCTION_NODE(TextRerankNode, TextRerankSpec());
}  // namespace llm_edgeflow

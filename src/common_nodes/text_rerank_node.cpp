#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "company_alg_log.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "engine/model_interface.h"
#include "nodes/model_bound_node.h"
#include "nodes/node_error_codes.h"
#include "nodes/traceable_batch_validation.h"

namespace alg_framework {

/**
 * @brief Cross-Encoder 语义精排通用算子 (TextRerankNode, 调用绑定的
 * IRerankModel)
 */
class TextRerankNode final : public ModelBoundNode<IRerankModel> {
 public:
  inline static constexpr char kNodeType[] = "TextRerankNode";

  TextRerankNode()
      : ModelBoundNode<IRerankModel>(kNodeType),
        in_queries_("queries"),
        in_candidates_("candidates"),
        in_candidate_texts_("candidate_texts"),
        in_pairs_("pairs"),
        out_ranked_("ranked") {}

 protected:
  bool InitModelNode(const NodeInitContext& init_ctx,
                     const nlohmann::json& config,
                     SessionContext& /*session_ctx*/) override {
    BindPort(init_ctx, in_queries_);
    BindPort(init_ctx, in_candidates_);
    BindPort(init_ctx, in_candidate_texts_);
    BindPort(init_ctx, in_pairs_);
    BindPort(init_ctx, out_ranked_);

    top_k_ = config.value("top_k", 1);
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* queries = in_queries_.Get(req_ctx);
    const auto* candidates = in_candidates_.Get(req_ctx);
    const auto* candidate_texts = in_candidate_texts_.Get(req_ctx);
    const auto* pairs = in_pairs_.Get(req_ctx);

    if (!pairs && !queries) {
      return node_error::text_rerank::kMissingInput;
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
        cand_payloads.push_back(
            {item.req_id, item.sub_id, item.data.candidate});
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
          pair_items.emplace_back(
              c.req_id, c.sub_id,
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

    if (pair_items.empty()) {
      out_ranked_.Set(req_ctx, RankedTextBatch{});
      return 0;
    }

    ScoreBatch pair_scores;
    ALG_LOG_DEBUG(
        "[TextRerankNode] Scoring %zu candidate (query, passage) "
        "pairs with Reranker...\n",
        pair_items.size());

    int ret = model()->Score(pair_items, &pair_scores);
    if (ret != 0) {
      return Fail(req_ctx, ret, "TextRerankNode: model scoring failed");
    }

    const auto alignment =
        ValidatePreservedTraceableAlignment(pair_items, pair_scores);
    if (alignment.error == TraceableAlignmentError::kCountMismatch) {
      return Fail(req_ctx, node_error::text_rerank::kModelOutputMismatch,
                  "TextRerankNode: score count mismatch");
    }
    if (alignment.error == TraceableAlignmentError::kProvenanceMismatch) {
      return Fail(req_ctx, node_error::text_rerank::kModelOutputMismatch,
                  "TextRerankNode: score provenance mismatch");
    }

    // 按 req_id 分组排序
    struct ScoredCandidate {
      uint32_t original_sub_id;
      std::string text;
      float score;
    };
    std::map<uint32_t, std::vector<ScoredCandidate>> req_scored_map;
    for (size_t i = 0; i < pair_scores.size() && i < cand_payloads.size();
         ++i) {
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
      size_t count = std::min(static_cast<size_t>(top_k_), list.size());
      for (size_t k = 0; k < count; ++k) {
        RankedCandidate rc(std::move(list[k].text), list[k].score,
                           static_cast<int>(k + 1), list[k].original_sub_id);
        refined_batch.emplace_back(r_id, static_cast<uint32_t>(k),
                                   std::move(rc));
      }
    }

    out_ranked_.Set(req_ctx, std::move(refined_batch));
    return 0;
  }

 private:
  size_t top_k_ = 1;

  BoundInput<TextBatch> in_queries_;
  BoundInput<RankedTextBatch> in_candidates_;
  BoundInput<TextBatch> in_candidate_texts_;
  BoundInput<QueryCandidatesBatch> in_pairs_;
  BoundOutput<RankedTextBatch> out_ranked_;
};

NodeDefinition MakeTextRerankNodeDefinition() {
  NodeDefinition def;
  def.node_type = TextRerankNode::kNodeType;
  def.category = "common";
  def.description = "Cross-encoder semantic reranking and top-k node";
  def.inputs = {
      OptionalInputPort("queries", BlackboardKey<TextBatch>{"", "TextBatch"},
                        "1:1", "preserve", "request"),
      OptionalInputPort("candidates",
                        BlackboardKey<RankedTextBatch>{"", "RankedTextBatch"},
                        "N:1", "preserve", "request"),
      OptionalInputPort("candidate_texts",
                        BlackboardKey<TextBatch>{"", "TextBatch"}, "N:1",
                        "preserve", "request"),
      OptionalInputPort(
          "pairs",
          BlackboardKey<QueryCandidatesBatch>{"", "QueryCandidatesBatch"},
          "1:1", "preserve", "request")};
  def.outputs = {OutputPort(
      "ranked", BlackboardKey<RankedTextBatch>{"", "RankedTextBatch"},
      /*allow_override=*/false, "1:N", "generate_sub_id", "request")};
  def.port_constraints = {PortGroupConstraint::Groups(
      PortConstraintKind::kExactOneGroupOf,
      {{"pairs"}, {"queries", "candidates"}, {"queries", "candidate_texts"}},
      "TextRerankNode requires exactly one input group: [pairs], [queries, "
      "candidates], or [queries, candidate_texts]")};
  def.config_fields = {
      ConfigFieldDefinition{"bind_model", ConfigValueKind::kString, false,
                            "rerank_model_v1"},
      ConfigFieldDefinition{"top_k", ConfigValueKind::kInteger, false, 1, 1.0,
                            1000.0}};
  def.model_capability = "rerank";
  def.model_config_field = "bind_model";
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(TextRerankNode, MakeTextRerankNodeDefinition());

}  // namespace alg_framework

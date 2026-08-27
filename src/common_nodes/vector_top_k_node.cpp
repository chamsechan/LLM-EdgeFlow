#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "nodes/node_support.h"

namespace alg_framework {

/**
 * @brief 向量相似度计算与 Top-K 检索排序算子 (VectorTopKNode)
 */
class VectorTopKNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "VectorTopKNode";

  VectorTopKNode()
      : NodeBase(kNodeType),
        in_queries_("queries", "queries", "EmbeddingBatch"),
        in_candidates_("candidates", "candidates", "EmbeddingBatch"),
        in_candidate_texts_("candidate_texts", "candidate_texts", "TextBatch"),
        out_ranked_("ranked", "ranked", "RankedTextBatch") {}

 protected:
  bool InitNode(const nlohmann::json& config,
                SessionContext& /*session_ctx*/) override {
    BindPort(in_queries_);
    BindPort(in_candidates_);
    BindPort(in_candidate_texts_);
    BindPort(out_ranked_);

    top_k_ = config.value("top_k", 1);
    min_score_ = config.value("min_score", 0.0f);
    metric_ = config.value("metric", "cosine");
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* queries =
        in_queries_.Require(req_ctx, -3101, "VectorTopKNode queries");
    const auto* candidates =
        in_candidates_.Require(req_ctx, -3101, "VectorTopKNode candidates");
    if (!queries || !candidates) {
      return -3101;
    }

    const auto* candidate_texts = in_candidate_texts_.Get(req_ctx);

    // 索引候选文本 (根据 req_id 和 sub_id)
    std::unordered_map<uint64_t, std::string> text_map;
    if (candidate_texts) {
      for (size_t i = 0; i < candidate_texts->size(); ++i) {
        const auto& ct = (*candidate_texts)[i];
        uint64_t key = (static_cast<uint64_t>(ct.req_id) << 32) | ct.sub_id;
        text_map[key] = ct.data;
      }
    }

    // 按 req_id 对候选向量分组
    std::unordered_map<uint32_t, std::vector<size_t>> req_candidate_indices;
    for (size_t i = 0; i < candidates->size(); ++i) {
      req_candidate_indices[(*candidates)[i].req_id].push_back(i);
    }

    // 检查是否为全局共享候选库 (即候选全部归属 req_id == 0，而 queries
    // 存在多请求)
    bool is_shared_candidates =
        (req_candidate_indices.size() == 1 &&
         req_candidate_indices.find(0) != req_candidate_indices.end() &&
         queries->size() > 1);

    RankedTextBatch ranked_batch;

    for (const auto& q_item : *queries) {
      uint32_t r_id = q_item.req_id;
      const auto& q_vec = q_item.data;

      const std::vector<size_t>* cand_indices = nullptr;
      if (req_candidate_indices.find(r_id) != req_candidate_indices.end()) {
        cand_indices = &req_candidate_indices[r_id];
      } else if (is_shared_candidates) {
        cand_indices = &req_candidate_indices[0];
      }

      if (!cand_indices || cand_indices->empty()) {
        continue;
      }

      struct ScoredItem {
        size_t cand_idx;
        uint32_t original_sub_id;
        float score;
        std::string text;
      };
      std::vector<ScoredItem> scored_list;
      scored_list.reserve(cand_indices->size());

      for (size_t c_idx : *cand_indices) {
        const auto& c_item = (*candidates)[c_idx];
        float score = (metric_ == "dot_product")
                          ? DotProduct(q_vec, c_item.data)
                          : CosineSimilarity(q_vec, c_item.data);
        if (score >= min_score_) {
          std::string text;
          uint64_t text_key =
              (static_cast<uint64_t>(c_item.req_id) << 32) | c_item.sub_id;
          if (text_map.find(text_key) != text_map.end()) {
            text = text_map[text_key];
          } else if (candidate_texts && c_idx < candidate_texts->size()) {
            text = (*candidate_texts)[c_idx].data;
          }
          scored_list.push_back({c_idx, c_item.sub_id, score, std::move(text)});
        }
      }

      std::sort(scored_list.begin(), scored_list.end(),
                [](const ScoredItem& a, const ScoredItem& b) {
                  return a.score > b.score;
                });

      size_t count = std::min(static_cast<size_t>(top_k_), scored_list.size());
      for (size_t k = 0; k < count; ++k) {
        const auto& item = scored_list[k];
        RankedCandidate rc(item.text, item.score, static_cast<int>(k + 1),
                           item.original_sub_id);
        ranked_batch.emplace_back(r_id, static_cast<uint32_t>(k),
                                  std::move(rc));
      }
    }

    std::cout << "[VectorTopKNode] Processed " << queries->size()
              << " queries, returned " << ranked_batch.size()
              << " top ranked items." << std::endl;

    out_ranked_.Set(req_ctx, std::move(ranked_batch));
    return 0;
  }

 private:
  static float CosineSimilarity(const std::vector<float>& v1,
                                const std::vector<float>& v2) {
    if (v1.empty() || v1.size() != v2.size()) return 0.0f;
    float dot = 0.0f, norm1 = 0.0f, norm2 = 0.0f;
    for (size_t i = 0; i < v1.size(); ++i) {
      dot += v1[i] * v2[i];
      norm1 += v1[i] * v1[i];
      norm2 += v2[i] * v2[i];
    }
    if (norm1 <= 0.0f || norm2 <= 0.0f) return 0.0f;
    return dot / (std::sqrt(norm1) * std::sqrt(norm2));
  }

  static float DotProduct(const std::vector<float>& v1,
                          const std::vector<float>& v2) {
    if (v1.empty() || v1.size() != v2.size()) return 0.0f;
    float dot = 0.0f;
    for (size_t i = 0; i < v1.size(); ++i) {
      dot += v1[i] * v2[i];
    }
    return dot;
  }

  size_t top_k_ = 1;
  float min_score_ = 0.0f;
  std::string metric_ = "cosine";

  BoundInput<EmbeddingBatch> in_queries_;
  BoundInput<EmbeddingBatch> in_candidates_;
  BoundInput<TextBatch> in_candidate_texts_;
  BoundOutput<RankedTextBatch> out_ranked_;
};

NodeDefinition MakeVectorTopKNodeDefinition() {
  NodeDefinition def;
  def.node_type = VectorTopKNode::kNodeType;
  def.category = "common";
  def.description = "Vector Top-K search and ranking node";
  def.inputs = {
      RequiredInputPort("queries",
                        BlackboardKey<EmbeddingBatch>{"", "EmbeddingBatch"}),
      RequiredInputPort("candidates",
                        BlackboardKey<EmbeddingBatch>{"", "EmbeddingBatch"}),
      OptionalInputPort("candidate_texts",
                        BlackboardKey<TextBatch>{"", "TextBatch"})};
  def.outputs = {OutputPort(
      "ranked", BlackboardKey<RankedTextBatch>{"", "RankedTextBatch"})};
  def.config_fields = {
      ConfigFieldDefinition{"top_k", ConfigValueKind::kInteger, false, 1, 1.0,
                            1000.0},
      ConfigFieldDefinition{"min_score", ConfigValueKind::kNumber, false, 0.0,
                            -100.0, 100.0},
      ConfigFieldDefinition{"metric",
                            ConfigValueKind::kString,
                            false,
                            "cosine",
                            std::nullopt,
                            std::nullopt,
                            {"cosine", "dot_product"}}};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(VectorTopKNode, MakeVectorTopKNodeDefinition());

}  // namespace alg_framework

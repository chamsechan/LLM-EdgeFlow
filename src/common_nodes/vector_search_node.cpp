#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>

#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"

namespace alg_framework {

/**
 * @brief 通用向量相似度检索与重排算子
 */
class VectorSearchNode : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    top_k_ = config.value("top_k", 1);
    min_score_ = config.value("min_score", 0.0f);
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* chunk_embeddings =
        req_ctx->Get<std::vector<TraceableItem<std::vector<float>>>>(
            "chunk_embeddings");
    auto* chunk_texts = req_ctx->Get<std::vector<TraceableItem<std::string>>>(
        "chunked_doc_items");
    auto* query_embeddings =
        req_ctx->Get<std::vector<TraceableItem<std::vector<float>>>>(
            "query_embeddings");

    if (!chunk_embeddings || !chunk_texts || !query_embeddings) {
      req_ctx->SetError(-3101,
                        "VectorSearchNode: Missing embeddings in context");
      return -3101;
    }

    // 按 req_id 分组候选分片
    std::map<uint32_t, std::vector<size_t>> req_to_chunk_indices;
    for (size_t i = 0; i < chunk_embeddings->size(); ++i) {
      req_to_chunk_indices[(*chunk_embeddings)[i].req_id].push_back(i);
    }

    std::vector<TraceableItem<std::string>> top_matched_chunks;

    // 对每一个请求计算与各分片的余弦相似度
    for (const auto& q_item : *query_embeddings) {
      uint32_t r_id = q_item.req_id;
      const auto& q_vec = q_item.data;

      if (req_to_chunk_indices.find(r_id) == req_to_chunk_indices.end()) {
        continue;
      }

      struct ScoredChunk {
        size_t idx;
        float score;
      };
      std::vector<ScoredChunk> scores;

      for (size_t c_idx : req_to_chunk_indices[r_id]) {
        const auto& c_vec = (*chunk_embeddings)[c_idx].data;
        float score = CosineSimilarity(q_vec, c_vec);
        scores.push_back({c_idx, score});
      }

      std::sort(scores.begin(), scores.end(),
                [](const ScoredChunk& a, const ScoredChunk& b) {
                  return a.score > b.score;
                });

      // 取 Top-K 结果
      for (size_t k = 0; k < std::min(top_k_, scores.size()); ++k) {
        size_t best_c_idx = scores[k].idx;
        const auto& src_chunk = (*chunk_texts)[best_c_idx];
        top_matched_chunks.emplace_back(r_id, src_chunk.sub_id, src_chunk.data);
      }
    }

    req_ctx->Set("matched_top_chunks", std::move(top_matched_chunks));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "VectorSearchNode";
    return name;
  }

 private:
  static float CosineSimilarity(const std::vector<float>& v1,
                                const std::vector<float>& v2) {
    if (v1.empty() || v1.size() != v2.size()) return 0.0f;
    float dot = 0.0f;
    for (size_t i = 0; i < v1.size(); ++i) {
      dot += v1[i] * v2[i];
    }
    return dot;  // 假设底层已做 L2 归一化
  }

 private:
  size_t top_k_ = 1;
  float min_score_ = 0.0f;
};

REGISTER_NODE(VectorSearchNode);

}  // namespace alg_framework

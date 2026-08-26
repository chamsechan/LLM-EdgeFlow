#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>

#include "business/doc_qa/doc_qa_contract.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "nodes/node_support.h"

namespace alg_framework {

/**
 * @brief 向量相似度检索与重排算子
 */
class VectorSearchNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "VectorSearchNode";

  VectorSearchNode() : NodeBase(kNodeType) {}

 protected:
  bool InitNode(const nlohmann::json& config,
                SessionContext& /*session_ctx*/) override {
    top_k_ = config.value("top_k", 1);
    min_score_ = config.value("min_score", 0.0f);
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* chunk_embeddings = Require(req_ctx, kChunkEmbeddings, -3101);
    const auto* chunk_texts = Require(req_ctx, kChunkedDocItems, -3101);
    const auto* query_embeddings = Require(req_ctx, kQueryEmbeddings, -3101);

    if (!chunk_embeddings || !chunk_texts || !query_embeddings) {
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
        if (score >= min_score_) {
          scores.push_back({c_idx, score});
        }
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

    Publish(req_ctx, kMatchedTopChunks, std::move(top_matched_chunks));
    return 0;
  }

 private:
  static float CosineSimilarity(const std::vector<float>& v1,
                                const std::vector<float>& v2) {
    if (v1.empty() || v1.size() != v2.size()) return 0.0f;
    float dot = 0.0f;
    for (size_t i = 0; i < v1.size(); ++i) {
      dot += v1[i] * v2[i];
    }
    float norm1 = 0.0f;
    float norm2 = 0.0f;
    for (float f : v1) norm1 += f * f;
    for (float f : v2) norm2 += f * f;
    if (norm1 <= 0.0f || norm2 <= 0.0f) return 0.0f;
    return dot / (std::sqrt(norm1) * std::sqrt(norm2));
  }

  size_t top_k_ = 1;
  float min_score_ = 0.0f;
};

NodeDefinition MakeVectorSearchNodeDefinition() {
  NodeDefinition def;
  def.node_type = VectorSearchNode::kNodeType;
  def.category = "business";
  def.description = "Vector search ranking node";
  def.inputs = {RequiredInput(kChunkEmbeddings),
                RequiredInput(kChunkedDocItems),
                RequiredInput(kQueryEmbeddings)};
  def.outputs = {Output(kMatchedTopChunks)};
  def.config_fields = {
      ConfigFieldDefinition{"top_k", ConfigValueKind::kInteger, false, 1, 1.0,
                            1000.0},
      ConfigFieldDefinition{"min_score", ConfigValueKind::kNumber, false, 0.0,
                            -1.0, 1.0},
      ConfigFieldDefinition{"metric",
                            ConfigValueKind::kString,
                            false,
                            "cosine",
                            std::nullopt,
                            std::nullopt,
                            {"cosine", "dot", "l2"}}};
  def.business_names = {kDocQaBusinessName, kDocQaOnnxBusinessName,
                        kDocQaRerankBusinessName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(VectorSearchNode,
                              MakeVectorSearchNodeDefinition());

}  // namespace alg_framework

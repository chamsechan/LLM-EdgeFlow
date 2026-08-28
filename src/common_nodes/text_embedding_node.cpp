#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "engine/engine_interface.h"
#include "nodes/model_bound_node.h"

namespace alg_framework {

/**
 * @brief 文本特征向量提取通用算子 (TextEmbeddingNode, 调用绑定的
 * IEmbeddingEngine) 支持 session 级别静态语料缓存与归一化
 */
class TextEmbeddingNode final : public ModelBoundNode<IEmbeddingEngine> {
 public:
  inline static constexpr char kNodeType[] = "TextEmbeddingNode";

  TextEmbeddingNode()
      : ModelBoundNode<IEmbeddingEngine>(kNodeType, "embed_model_v1"),
        in_text_("text"),
        out_embedding_("embedding") {}

 protected:
  bool InitModelNode(const NodeInitContext& init_ctx,
                     const nlohmann::json& config,
                     SessionContext& session_ctx) override {
    BindPort(init_ctx, in_text_);
    BindPort(init_ctx, out_embedding_);
    normalize_ = config.value("normalize", true);
    lifetime_ = config.value("lifetime", "request");
    bind_model_id_ = config.value("bind_model", "embed_model_v1");
    session_ctx_ = &session_ctx;
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* text_items =
        in_text_.Require(req_ctx, -4101, "TextEmbeddingNode input");
    if (!text_items) {
      return -4101;
    }

    if (text_items->empty()) {
      out_embedding_.Set(req_ctx, EmbeddingBatch{});
      return 0;
    }

    if (lifetime_ == "session" && session_ctx_) {
      std::string digest = ComputeDigest(*text_items);
      std::string cache_key = "static_emb:" + bind_model_id_ + ":" +
                              std::to_string(normalize_) + ":" + digest;
      int infer_err = 0;
      auto cached = session_ctx_->GetOrCreateResource<EmbeddingBatch>(
          cache_key, [&]() -> std::shared_ptr<EmbeddingBatch> {
            auto output = std::make_shared<EmbeddingBatch>();
            int ret = engine()->InferTraceableBatch(*text_items, output.get());
            if (ret != 0) {
              infer_err = ret;
              return nullptr;
            }
            if (normalize_) {
              Normalize(*output);
            }
            return output;
          });
      if (!cached) {
        return Fail(req_ctx, infer_err != 0 ? infer_err : -5101,
                    "TextEmbeddingNode: single-flight inference failed");
      }
      out_embedding_.Set(req_ctx, *cached);
      return 0;
    }

    EmbeddingBatch output_embeddings;
    std::cout << "[TextEmbeddingNode] Inferring embeddings for "
              << text_items->size() << " text items using engine..."
              << std::endl;

    int ret = engine()->InferTraceableBatch(*text_items, &output_embeddings);
    if (ret != 0) {
      return Fail(req_ctx, ret, "TextEmbeddingNode: inference failed");
    }

    if (normalize_) {
      Normalize(output_embeddings);
    }

    out_embedding_.Set(req_ctx, std::move(output_embeddings));
    return 0;
  }

 private:
  static void Normalize(EmbeddingBatch& batch) {
    for (auto& item : batch) {
      float norm = 0.0f;
      for (float v : item.data) norm += v * v;
      if (norm > 0.0f) {
        float inv = 1.0f / std::sqrt(norm);
        for (float& v : item.data) v *= inv;
      }
    }
  }

  static std::string ComputeDigest(const TextBatch& items) {
    size_t hash = 14695981039346656037ULL;
    for (const auto& item : items) {
      hash ^= static_cast<size_t>(item.req_id);
      hash *= 1099511628211ULL;
      hash ^= static_cast<size_t>(item.sub_id);
      hash *= 1099511628211ULL;
      for (char c : item.data) {
        hash ^= static_cast<size_t>(c);
        hash *= 1099511628211ULL;
      }
    }
    return std::to_string(hash);
  }

  bool normalize_ = true;
  std::string lifetime_ = "request";
  std::string bind_model_id_ = "embed_model_v1";
  SessionContext* session_ctx_ = nullptr;

  BoundInput<TextBatch> in_text_;
  BoundOutput<EmbeddingBatch> out_embedding_;
};

NodeDefinition MakeTextEmbeddingNodeDefinition() {
  NodeDefinition def;
  def.node_type = TextEmbeddingNode::kNodeType;
  def.category = "common";
  def.description = "Text embedding extraction node";
  def.inputs = {RequiredInputPort("text",
                                  BlackboardKey<TextBatch>{"", "TextBatch"},
                                  "1:1", "preserve", "request")};
  def.outputs = {OutputPort("embedding",
                            BlackboardKey<EmbeddingBatch>{"", "EmbeddingBatch"},
                            false, "1:1", "preserve", "request")};
  def.config_fields = {
      ConfigFieldDefinition{"bind_model", ConfigValueKind::kString, false,
                            "embed_model_v1"},
      ConfigFieldDefinition{"normalize", ConfigValueKind::kBoolean, false,
                            true},
      ConfigFieldDefinition{"lifetime",
                            ConfigValueKind::kString,
                            false,
                            "request",
                            std::nullopt,
                            std::nullopt,
                            {"request", "session"}}};
  def.model_capability = "embedding";
  def.model_config_field = "bind_model";
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(TextEmbeddingNode,
                              MakeTextEmbeddingNodeDefinition());

}  // namespace alg_framework

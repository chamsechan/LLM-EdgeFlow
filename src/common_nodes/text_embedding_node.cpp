#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "company_alg_log.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "engine/model_interface.h"
#include "nodes/model_bound_node.h"
#include "nodes/node_error_codes.h"
#include "nodes/traceable_batch_validation.h"

namespace llm_edgeflow {

/**
 * @brief 文本特征向量提取通用算子 (TextEmbeddingNode, 调用绑定的
 * IEmbeddingModel) 支持 session 级别静态语料缓存与归一化
 */
class TextEmbeddingNode final : public ModelBoundNode<IEmbeddingModel> {
 public:
  inline static constexpr char kNodeType[] = "TextEmbeddingNode";

  TextEmbeddingNode()
      : ModelBoundNode<IEmbeddingModel>(kNodeType),
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
    if (lifetime_ != "request" && lifetime_ != "session") return false;
    bind_model_id_ = model_id();
    session_ctx_ = &session_ctx;
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* text_items =
        in_text_.Require(req_ctx, node_error::text_embedding::kMissingInput,
                         "TextEmbeddingNode input");
    if (!text_items) {
      return node_error::text_embedding::kMissingInput;
    }

    if (text_items->empty()) {
      out_embedding_.Set(req_ctx, EmbeddingBatch{});
      return 0;
    }

    EmbeddingOptions opts;
    opts.normalize = normalize_;

    if (lifetime_ == "session" && session_ctx_) {
      std::string digest = ComputeDigest(*text_items);
      const std::string model_revision =
          session_ctx_->GetModelManager().GetModelRevision(bind_model_id_);
      std::string cache_key = "static_emb:" + bind_model_id_ + ":" +
                              model_revision + ":" +
                              std::to_string(normalize_) + ":" + digest;
      int infer_err = 0;
      auto cached = session_ctx_->GetOrCreateResource<EmbeddingBatch>(
          cache_key, [&]() -> std::shared_ptr<EmbeddingBatch> {
            auto output = std::make_shared<EmbeddingBatch>();
            int ret = model()->Embed(*text_items, opts, output.get());
            if (ret != 0) {
              infer_err = ret;
              return nullptr;
            }
            const auto alignment =
                ValidatePreservedTraceableAlignment(*text_items, *output);
            if (!alignment.IsAligned()) {
              infer_err = AlignmentErrorCode(alignment.error);
              return nullptr;
            }
            return output;
          });
      if (!cached) {
        if (infer_err == node_error::text_embedding::kOutputCountMismatch ||
            infer_err ==
                node_error::text_embedding::kOutputProvenanceMismatch) {
          return FailAlignment(req_ctx, infer_err);
        }
        return Fail(req_ctx,
                    infer_err != 0
                        ? infer_err
                        : node_error::text_embedding::kSessionInferenceFailed,
                    "TextEmbeddingNode: single-flight inference failed");
      }
      out_embedding_.Set(req_ctx, *cached);
      return 0;
    }

    EmbeddingBatch output_embeddings;
    ALG_LOG_DEBUG(
        "[TextEmbeddingNode] Inferring embeddings for %zu text "
        "items using model...\n",
        text_items->size());

    int ret = model()->Embed(*text_items, opts, &output_embeddings);
    if (ret != 0) {
      return Fail(req_ctx, ret, "TextEmbeddingNode: inference failed");
    }
    const auto alignment =
        ValidatePreservedTraceableAlignment(*text_items, output_embeddings);
    if (!alignment.IsAligned()) {
      return FailAlignment(req_ctx, AlignmentErrorCode(alignment.error));
    }

    out_embedding_.Set(req_ctx, std::move(output_embeddings));
    return 0;
  }

 private:
  static int AlignmentErrorCode(TraceableAlignmentError error) noexcept {
    switch (error) {
      case TraceableAlignmentError::kCountMismatch:
        return node_error::text_embedding::kOutputCountMismatch;
      case TraceableAlignmentError::kProvenanceMismatch:
        return node_error::text_embedding::kOutputProvenanceMismatch;
      case TraceableAlignmentError::kNone:
        return 0;
    }
    return 0;
  }

  int FailAlignment(AlgContext& req_ctx, int error_code) const noexcept {
    return Fail(req_ctx, error_code,
                error_code == node_error::text_embedding::kOutputCountMismatch
                    ? "TextEmbeddingNode: embedding count mismatch"
                    : "TextEmbeddingNode: embedding provenance mismatch");
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
  std::string bind_model_id_;
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
                                  "N:M", "preserve", "request", "lifetime")};
  def.outputs = {OutputPort("embedding",
                            BlackboardKey<EmbeddingBatch>{"", "EmbeddingBatch"},
                            "N:M", "preserve", "request", "lifetime")};
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

}  // namespace llm_edgeflow

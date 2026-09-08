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
      const std::string model_revision =
          session_ctx_->GetModelManager().GetModelRevision(bind_model_id_);
      std::string cache_key = ConstructSessionCacheKey(
          bind_model_id_, model_revision, normalize_, *text_items);
      SessionResourceKey<EmbeddingBatch> resource_key(std::move(cache_key));
      int infer_err = 0;
      auto cached = session_ctx_->GetOrCreateResource<EmbeddingBatch>(
          resource_key, [&]() -> std::shared_ptr<EmbeddingBatch> {
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
      const auto hit_alignment =
          ValidatePreservedTraceableAlignment(*text_items, *cached);
      if (!hit_alignment.IsAligned()) {
        return FailAlignment(req_ctx, AlignmentErrorCode(hit_alignment.error));
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

  static void AppendUint64Le(std::string& buf, uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      buf.push_back(static_cast<char>((value >> shift) & 0xff));
    }
  }

  static std::string ConstructSessionCacheKey(const std::string& model_id,
                                              const std::string& revision,
                                              bool normalize,
                                              const TextBatch& items) {
    std::string key;
    key.append("SEM1", 4);
    AppendUint64Le(key, model_id.size());
    key.append(model_id.data(), model_id.size());
    AppendUint64Le(key, revision.size());
    key.append(revision.data(), revision.size());
    key.push_back(normalize ? '\x01' : '\x00');
    AppendUint64Le(key, items.size());
    for (const auto& item : items) {
      AppendUint64Le(key, item.req_id);
      AppendUint64Le(key, item.sub_id);
      AppendUint64Le(key, item.data.size());
      key.append(item.data.data(), item.data.size());
    }
    return key;
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
      ConfigFieldDefinition{
          "bind_model",
          ConfigValueKind::kString,
          false,
          "embed_model_v1",
          std::nullopt,
          std::nullopt,
          {},
          "引用 models[].model_id；所选模型必须提供 embedding 文本向量能力。"},
      ConfigFieldDefinition{"normalize",
                            ConfigValueKind::kBoolean,
                            false,
                            true,
                            std::nullopt,
                            std::nullopt,
                            {},
                            "要求模型对输出向量做 L2 归一化。"},
      ConfigFieldDefinition{"lifetime",
                            ConfigValueKind::kString,
                            false,
                            "request",
                            std::nullopt,
                            std::nullopt,
                            {"request", "session"},
                            "request 每次请求计算；session "
                            "按模型版本、归一化选项和输入缓存向量，输入须满足 "
                            "session 生命周期契约。"}};
  def.model_capability = "embedding";
  def.model_config_field = "bind_model";
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(TextEmbeddingNode,
                              MakeTextEmbeddingNodeDefinition());

}  // namespace llm_edgeflow

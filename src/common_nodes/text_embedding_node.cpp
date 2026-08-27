#include <cmath>
#include <iostream>
#include <vector>

#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "engine/engine_interface.h"
#include "nodes/model_bound_node.h"

namespace alg_framework {

/**
 * @brief 文本特征向量提取通用算子 (TextEmbeddingNode, 调用绑定的
 * IEmbeddingEngine)
 */
class TextEmbeddingNode final : public ModelBoundNode<IEmbeddingEngine> {
 public:
  inline static constexpr char kNodeType[] = "TextEmbeddingNode";

  TextEmbeddingNode()
      : ModelBoundNode<IEmbeddingEngine>(kNodeType, "embed_model_v1"),
        in_text_("text", "text", "TextBatch"),
        out_embedding_("embedding", "embedding", "EmbeddingBatch") {}

 protected:
  bool InitModelNode(const nlohmann::json& config,
                     SessionContext& /*session_ctx*/) override {
    BindPort(in_text_);
    BindPort(out_embedding_);
    normalize_ = config.value("normalize", true);
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

    EmbeddingBatch output_embeddings;
    std::cout << "[TextEmbeddingNode] Inferring embeddings for "
              << text_items->size() << " text items using engine..."
              << std::endl;

    int ret = engine()->InferTraceableBatch(*text_items, &output_embeddings);
    if (ret != 0) {
      return Fail(req_ctx, ret, "TextEmbeddingNode: inference failed");
    }

    if (normalize_) {
      for (auto& item : output_embeddings) {
        float norm = 0.0f;
        for (float v : item.data) norm += v * v;
        if (norm > 0.0f) {
          float inv = 1.0f / std::sqrt(norm);
          for (float& v : item.data) v *= inv;
        }
      }
    }

    out_embedding_.Set(req_ctx, std::move(output_embeddings));
    return 0;
  }

 private:
  bool normalize_ = true;
  BoundInput<TextBatch> in_text_;
  BoundOutput<EmbeddingBatch> out_embedding_;
};

NodeDefinition MakeTextEmbeddingNodeDefinition() {
  NodeDefinition def;
  def.node_type = TextEmbeddingNode::kNodeType;
  def.category = "common";
  def.description = "Text embedding extraction node";
  def.inputs = {
      RequiredInputPort("text", BlackboardKey<TextBatch>{"", "TextBatch"})};
  def.outputs = {OutputPort(
      "embedding", BlackboardKey<EmbeddingBatch>{"", "EmbeddingBatch"})};
  def.config_fields = {
      ConfigFieldDefinition{"bind_model", ConfigValueKind::kString, false,
                            "embed_model_v1"},
      ConfigFieldDefinition{"normalize", ConfigValueKind::kBoolean, false,
                            true}};
  def.model_capability = "embedding";
  def.model_config_field = "bind_model";
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(TextEmbeddingNode,
                              MakeTextEmbeddingNodeDefinition());

}  // namespace alg_framework

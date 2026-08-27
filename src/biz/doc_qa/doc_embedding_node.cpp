#include <iostream>

#include "biz/doc_qa/doc_qa_contract.h"
#include "core/node_registry.h"
#include "engine/engine_interface.h"
#include "nodes/model_bound_node.h"

namespace alg_framework {

/**
 * @brief 文档与问题向量提取算子 (调用绑定的 Embedding 引擎)
 */
class DocEmbeddingNode final : public ModelBoundNode<IEmbeddingEngine> {
 public:
  inline static constexpr char kNodeType[] = "DocEmbeddingNode";

  DocEmbeddingNode()
      : ModelBoundNode<IEmbeddingEngine>(kNodeType, "embed_model_v1") {}

 protected:
  int ProcessNode(AlgContext& req_ctx) override {
    const auto* chunk_items = Require(req_ctx, kChunkedDocItems, -4101);
    const auto* query_items = Require(req_ctx, kQueryItems, -4101);
    if (!chunk_items || !query_items) {
      return -4101;
    }

    std::vector<TraceableItem<std::vector<float>>> chunk_embeddings;
    std::vector<TraceableItem<std::vector<float>>> query_embeddings;

    std::cout << "[DocEmbeddingNode] Inferring embeddings for "
              << chunk_items->size() << " doc chunks..." << std::endl;
    int ret = engine()->InferTraceableBatch(*chunk_items, &chunk_embeddings);
    if (ret != 0) {
      return Fail(req_ctx, ret,
                  "DocEmbeddingNode: Chunk embedding inference failed");
    }

    std::cout << "[DocEmbeddingNode] Inferring embeddings for "
              << query_items->size() << " queries..." << std::endl;
    ret = engine()->InferTraceableBatch(*query_items, &query_embeddings);
    if (ret != 0) {
      return Fail(req_ctx, ret,
                  "DocEmbeddingNode: Query embedding inference failed");
    }

    Publish(req_ctx, kChunkEmbeddings, std::move(chunk_embeddings));
    Publish(req_ctx, kQueryEmbeddings, std::move(query_embeddings));
    return 0;
  }
};

NodeDefinition MakeDocEmbeddingNodeDefinition() {
  NodeDefinition def;
  def.node_type = DocEmbeddingNode::kNodeType;
  def.category = "biz";
  def.description = "Document embedding extraction node";
  def.inputs = {RequiredInput(kChunkedDocItems), RequiredInput(kQueryItems)};
  def.outputs = {Output(kChunkEmbeddings), Output(kQueryEmbeddings)};
  def.config_fields = {ConfigFieldDefinition{
      "bind_model", ConfigValueKind::kString, false, "embed_model_v1"}};
  def.model_capability = "embedding";
  def.model_config_field = "bind_model";
  def.biz_names = {kDocQaBizName, kDocQaOnnxBizName, kDocQaRerankBizName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(DocEmbeddingNode,
                              MakeDocEmbeddingNodeDefinition());

}  // namespace alg_framework

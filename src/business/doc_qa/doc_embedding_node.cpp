#include <iostream>

#include "business/doc_qa/doc_qa_contract.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "engine/engine_interface.h"

namespace alg_framework {

/**
 * @brief 文档与问题向量提取算子 (调用绑定的 Embedding 引擎)
 */
class DocEmbeddingNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "DocEmbeddingNode";

  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    std::string bind_model_id = config.value("bind_model", "embed_model_v1");
    engine_ = session_ctx->GetModelManager().GetModel<IEmbeddingEngine>(
        bind_model_id);
    if (!engine_) {
      std::cerr << "[DocEmbeddingNode] Failed to get model: " << bind_model_id
                << std::endl;
      return false;
    }
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* chunk_items = req_ctx->Get(kChunkedDocItems);
    auto* query_items = req_ctx->Get(kQueryItems);

    if (!chunk_items || !query_items) {
      req_ctx->SetError(-4101,
                        "DocEmbeddingNode: Missing chunk_items or query_items");
      return -4101;
    }

    std::vector<TraceableItem<std::vector<float>>> chunk_embeddings;
    std::vector<TraceableItem<std::vector<float>>> query_embeddings;

    std::cout << "[DocEmbeddingNode] Inferring embeddings for "
              << chunk_items->size() << " doc chunks..." << std::endl;
    int ret = engine_->InferTraceableBatch(*chunk_items, &chunk_embeddings);
    if (ret != 0) {
      req_ctx->SetError(ret,
                        "DocEmbeddingNode: Chunk embedding inference failed");
      return ret;
    }

    std::cout << "[DocEmbeddingNode] Inferring embeddings for "
              << query_items->size() << " queries..." << std::endl;
    ret = engine_->InferTraceableBatch(*query_items, &query_embeddings);
    if (ret != 0) {
      req_ctx->SetError(ret,
                        "DocEmbeddingNode: Query embedding inference failed");
      return ret;
    }

    req_ctx->Set(kChunkEmbeddings, std::move(chunk_embeddings));
    req_ctx->Set(kQueryEmbeddings, std::move(query_embeddings));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = kNodeType;
    return name;
  }

 private:
  std::shared_ptr<IEmbeddingEngine> engine_;
};

NodeDefinition MakeDocEmbeddingNodeDefinition() {
  NodeDefinition def;
  def.node_type = DocEmbeddingNode::kNodeType;
  def.category = "business";
  def.description = "Document embedding extraction node";
  def.inputs = {RequiredInput(kChunkedDocItems), RequiredInput(kQueryItems)};
  def.outputs = {Output(kChunkEmbeddings), Output(kQueryEmbeddings)};
  def.config_fields = {ConfigFieldDefinition{
      "bind_model", ConfigValueKind::kString, false, "embedding_model_v1"}};
  def.model_capability = "embedding";
  def.model_config_field = "bind_model";
  def.business_names = {"doc_qa_embedding_v1", "doc_qa_rerank_v1"};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(DocEmbeddingNode,
                              MakeDocEmbeddingNodeDefinition());

}  // namespace alg_framework

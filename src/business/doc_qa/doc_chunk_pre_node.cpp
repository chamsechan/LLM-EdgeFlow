#include <iostream>

#include "business/doc_qa/doc_qa_contract.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"

namespace alg_framework {

/**
 * @brief 文档分块前处理算子 (1对N裂变)
 */
class DocChunkPreNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "DocChunkPreNode";

  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    chunk_size_ = config.value("chunk_size", 100);
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* raw_docs = req_ctx->Get(kRawDocs);
    auto* raw_queries = req_ctx->Get(kRawQueries);

    if (!raw_docs || !raw_queries) {
      req_ctx->SetError(-4001,
                        "DocChunkPreNode: Missing raw_docs or raw_queries");
      return -4001;
    }

    std::vector<TraceableItem<std::string>> chunked_doc_items;
    std::vector<TraceableItem<std::string>> query_items;
    std::vector<int> chunk_counts_per_req(raw_docs->size(), 0);

    // 1. 文档分块 (1对N裂变并打上溯源标签)
    for (uint32_t req_id = 0; req_id < raw_docs->size(); ++req_id) {
      const std::string& doc = (*raw_docs)[req_id];
      uint32_t sub_id = 0;

      if (doc.empty()) {
        chunked_doc_items.emplace_back(req_id, sub_id++, "");
      } else {
        for (size_t pos = 0; pos < doc.size(); pos += chunk_size_) {
          std::string slice = doc.substr(pos, chunk_size_);
          chunked_doc_items.emplace_back(req_id, sub_id++, slice);
        }
      }
      chunk_counts_per_req[req_id] = sub_id;
    }

    // 2. Query 包装为 TraceableItem
    for (uint32_t req_id = 0; req_id < raw_queries->size(); ++req_id) {
      query_items.emplace_back(req_id, 0, (*raw_queries)[req_id]);
    }

    std::cout << "[DocChunkPreNode] Generated " << chunked_doc_items.size()
              << " total chunks from " << raw_docs->size() << " requests."
              << std::endl;

    req_ctx->Set(kChunkedDocItems, std::move(chunked_doc_items));
    req_ctx->Set(kQueryItems, std::move(query_items));
    req_ctx->Set(kChunkCountsPerReq, std::move(chunk_counts_per_req));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = kNodeType;
    return name;
  }

 private:
  size_t chunk_size_ = 100;
};

NodeDefinition MakeDocChunkPreNodeDefinition() {
  NodeDefinition def;
  def.node_type = DocChunkPreNode::kNodeType;
  def.category = "business";
  def.description = "Document chunk pre-processing node";
  def.inputs = {RequiredInput(kRawDocs), RequiredInput(kRawQueries)};
  def.outputs = {Output(kChunkedDocItems), Output(kQueryItems),
                 Output(kChunkCountsPerReq)};
  def.config_fields = {ConfigFieldDefinition{
      "chunk_size", ConfigValueKind::kInteger, false, 100, 1.0, 100000.0}};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(DocChunkPreNode, MakeDocChunkPreNodeDefinition());

}  // namespace alg_framework

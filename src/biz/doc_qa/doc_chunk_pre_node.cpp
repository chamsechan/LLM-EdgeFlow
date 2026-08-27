#include <iostream>

#include "biz/doc_qa/doc_qa_contract.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "nodes/node_support.h"

namespace alg_framework {

/**
 * @brief 文档分块前处理算子 (1对N裂变)
 */
class DocChunkPreNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "DocChunkPreNode";

  DocChunkPreNode() : NodeBase(kNodeType) {}

 protected:
  bool InitNode(const nlohmann::json& config,
                SessionContext& /*session_ctx*/) override {
    chunk_size_ = config.value("chunk_size", 100);
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* raw_docs = Require(req_ctx, kRawDocs, -4001);
    const auto* raw_queries = Require(req_ctx, kRawQueries, -4001);
    if (!raw_docs || !raw_queries) {
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

    Publish(req_ctx, kChunkedDocItems, std::move(chunked_doc_items));
    Publish(req_ctx, kQueryItems, std::move(query_items));
    Publish(req_ctx, kChunkCountsPerReq, std::move(chunk_counts_per_req));
    return 0;
  }

 private:
  size_t chunk_size_ = 100;
};

NodeDefinition MakeDocChunkPreNodeDefinition() {
  NodeDefinition def;
  def.node_type = DocChunkPreNode::kNodeType;
  def.category = "biz";
  def.description = "Document chunk pre-processing node";
  def.inputs = {RequiredInput(kRawDocs), RequiredInput(kRawQueries)};
  def.outputs = {Output(kChunkedDocItems), Output(kQueryItems),
                 Output(kChunkCountsPerReq)};
  def.config_fields = {ConfigFieldDefinition{
      "chunk_size", ConfigValueKind::kInteger, false, 100, 1.0, 100000.0}};
  def.biz_names = {kDocQaBizName, kDocQaOnnxBizName, kDocQaRerankBizName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(DocChunkPreNode, MakeDocChunkPreNodeDefinition());

}  // namespace alg_framework

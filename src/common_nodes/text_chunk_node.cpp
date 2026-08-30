#include <algorithm>
#include <string>
#include <vector>

#include "company_alg_log.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "nodes/node_error_codes.h"
#include "nodes/node_support.h"

namespace alg_framework {

/**
 * @brief 文本切片分块算子 (TextChunkNode, 1对N裂变与溯源绑定)
 */
class TextChunkNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "TextChunkNode";

  TextChunkNode()
      : NodeBase(kNodeType),
        in_text_("text"),
        out_chunks_("chunks"),
        out_chunk_counts_("chunk_counts") {}

 protected:
  bool InitNode(const NodeInitContext& init_ctx, const nlohmann::json& config,
                SessionContext& /*session_ctx*/) override {
    BindPort(init_ctx, in_text_);
    BindPort(init_ctx, out_chunks_);
    BindPort(init_ctx, out_chunk_counts_);

    chunk_size_ = config.value("chunk_size", 100);
    overlap_ = config.value("overlap", 0);
    if (chunk_size_ == 0) chunk_size_ = 100;
    if (overlap_ >= chunk_size_) overlap_ = 0;
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* text_items = in_text_.Require(
        req_ctx, node_error::text_chunk::kMissingInput, "TextChunkNode input");
    if (!text_items) {
      return node_error::text_chunk::kMissingInput;
    }

    TextBatch chunked_items;
    Int32Batch chunk_counts;
    chunk_counts.reserve(text_items->size());
    size_t step =
        (chunk_size_ > overlap_) ? (chunk_size_ - overlap_) : chunk_size_;

    for (const auto& item : *text_items) {
      const std::string& str = item.data;
      uint32_t req_id = item.req_id;
      uint32_t sub_id = 0;
      int32_t count_for_req = 0;

      if (str.empty()) {
        chunked_items.emplace_back(req_id, sub_id++, "");
        count_for_req = 1;
      } else {
        for (size_t pos = 0; pos < str.size(); pos += step) {
          std::string slice = str.substr(pos, chunk_size_);
          chunked_items.emplace_back(req_id, sub_id++, std::move(slice));
          count_for_req++;
        }
      }
      chunk_counts.emplace_back(req_id, 0, count_for_req);
    }

    ALG_LOG_DEBUG("[TextChunkNode] Split %zu input texts into %zu chunks.\n",
                  text_items->size(), chunked_items.size());

    out_chunks_.Set(req_ctx, std::move(chunked_items));
    out_chunk_counts_.Set(req_ctx, std::move(chunk_counts));
    return 0;
  }

 private:
  size_t chunk_size_ = 100;
  size_t overlap_ = 0;

  BoundInput<TextBatch> in_text_;
  BoundOutput<TextBatch> out_chunks_;
  BoundOutput<Int32Batch> out_chunk_counts_;
};

NodeDefinition MakeTextChunkNodeDefinition() {
  NodeDefinition def;
  def.node_type = TextChunkNode::kNodeType;
  def.category = "common";
  def.description = "Text chunking and slicing pre-processing node";
  def.inputs = {RequiredInputPort("text",
                                  BlackboardKey<TextBatch>{"", "TextBatch"},
                                  "1:1", "preserve", "request")};
  def.outputs = {
      OutputPort("chunks", BlackboardKey<TextBatch>{"", "TextBatch"}, false,
                 "1:N", "generate_sub_id", "request"),
      OutputPort("chunk_counts", BlackboardKey<Int32Batch>{"", "Int32Batch"},
                 /*allow_override=*/false, "1:1", "preserve", "request")};
  def.config_fields = {
      ConfigFieldDefinition{"chunk_size", ConfigValueKind::kInteger, false, 100,
                            1.0, 1000000.0},
      ConfigFieldDefinition{"overlap", ConfigValueKind::kInteger, false, 0, 0.0,
                            100000.0}};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(TextChunkNode, MakeTextChunkNodeDefinition());

}  // namespace alg_framework

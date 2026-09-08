#include <algorithm>
#include <limits>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "company_alg_log.h"
#include "contracts/config_schema_validation.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "engine/text/utf8.h"
#include "nodes/node_base.h"
#include "nodes/node_error_codes.h"

namespace llm_edgeflow {

namespace {
constexpr int64_t kDefaultChunkSize = 100;
constexpr int64_t kDefaultOverlap = 0;

const std::vector<ConfigFieldDefinition>& TextChunkConfigFields() {
  static const std::vector<ConfigFieldDefinition> kFields = {
      ConfigFieldDefinition{"chunk_size",
                            ConfigValueKind::kInteger,
                            false,
                            kDefaultChunkSize,
                            1.0,
                            1000000.0,
                            {},
                            "每块最多包含的 Unicode 码点数；按 UTF-8 "
                            "字符边界切分，不是字节数或模型 token 数。"},
      ConfigFieldDefinition{
          "overlap",
          ConfigValueKind::kInteger,
          false,
          kDefaultOverlap,
          0.0,
          100000.0,
          {},
          "相邻块重叠的 Unicode 码点数，必须小于 chunk_size；0 表示无重叠。"}};
  return kFields;
}

bool ValidChunkConfig(const nlohmann::json& config) {
  const auto size = config.value<int64_t>("chunk_size", kDefaultChunkSize);
  const auto overlap = config.value<int64_t>("overlap", kDefaultOverlap);
  return size > 0 && size <= 1000000 && overlap >= 0 && overlap <= 100000 &&
         overlap < size;
}
}  // namespace

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
    nlohmann::json normalized;
    if (!ValidateAndNormalizeFields(TextChunkConfigFields(), config,
                                    &normalized, nullptr)) {
      return false;
    }
    if (!ValidChunkConfig(normalized)) {
      return false;
    }

    BindPort(init_ctx, in_text_);
    BindPort(init_ctx, out_chunks_);
    BindPort(init_ctx, out_chunk_counts_);

    chunk_size_ = static_cast<size_t>(normalized["chunk_size"].get<int64_t>());
    overlap_ = static_cast<size_t>(normalized["overlap"].get<int64_t>());
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* text_items = in_text_.Require(
        req_ctx, node_error::text_chunk::kMissingInput, "TextChunkNode input");
    if (!text_items) {
      return node_error::text_chunk::kMissingInput;
    }

    std::set<std::pair<uint32_t, uint32_t>> seen_inputs;
    for (const auto& item : *text_items) {
      if (!seen_inputs.insert({item.req_id, item.sub_id}).second) {
        return Fail(req_ctx, node_error::text_chunk::kDuplicateInput,
                    "TextChunkNode duplicate input item for req_id=" +
                        std::to_string(item.req_id) +
                        ", sub_id=" + std::to_string(item.sub_id));
      }
    }

    TextBatch chunked_items;
    Int32Batch chunk_counts;
    chunk_counts.reserve(text_items->size());
    size_t step =
        (chunk_size_ > overlap_) ? (chunk_size_ - overlap_) : chunk_size_;

    std::unordered_map<uint32_t, uint64_t> next_sub_id_by_req;

    for (const auto& item : *text_items) {
      const std::string& str = item.data;
      uint32_t req_id = item.req_id;
      uint64_t& next_sub_id = next_sub_id_by_req[req_id];
      int32_t count_for_req = 0;

      if (str.empty()) {
        if (next_sub_id > std::numeric_limits<uint32_t>::max()) {
          return Fail(req_ctx, node_error::text_chunk::kSubIdOverflow,
                      "TextChunkNode sub_id overflow for req_id=" +
                          std::to_string(req_id));
        }
        chunked_items.emplace_back(req_id, static_cast<uint32_t>(next_sub_id++),
                                   "");
        count_for_req = 1;
      } else {
        std::vector<size_t> boundaries;
        size_t invalid_offset = 0;
        if (!utf8::BuildCodePointBoundaries(str, &boundaries,
                                            &invalid_offset)) {
          return Fail(req_ctx, node_error::text_chunk::kInvalidUtf8,
                      "TextChunkNode invalid UTF-8 input for req_id=" +
                          std::to_string(req_id) + " at byte offset " +
                          std::to_string(invalid_offset));
        }

        const size_t code_point_count = boundaries.size() - 1;
        for (size_t pos = 0; pos < code_point_count; pos += step) {
          const size_t end = std::min(pos + chunk_size_, code_point_count);
          std::string slice =
              str.substr(boundaries[pos], boundaries[end] - boundaries[pos]);
          if (next_sub_id > std::numeric_limits<uint32_t>::max()) {
            return Fail(req_ctx, node_error::text_chunk::kSubIdOverflow,
                        "TextChunkNode sub_id overflow for req_id=" +
                            std::to_string(req_id));
          }
          if (count_for_req == std::numeric_limits<int32_t>::max()) {
            return Fail(req_ctx, node_error::text_chunk::kCountOverflow,
                        "TextChunkNode chunk count exceeds Int32 capacity");
          }
          chunked_items.emplace_back(
              req_id, static_cast<uint32_t>(next_sub_id++), std::move(slice));
          count_for_req++;
          if (end == code_point_count) break;
        }
      }
      chunk_counts.emplace_back(req_id, item.sub_id, count_for_req);
    }

    ALG_LOG_DEBUG("[TextChunkNode] Split %zu input texts into %zu chunks.\n",
                  text_items->size(), chunked_items.size());

    out_chunks_.Set(req_ctx, std::move(chunked_items));
    out_chunk_counts_.Set(req_ctx, std::move(chunk_counts));
    return 0;
  }

 private:
  size_t chunk_size_ = kDefaultChunkSize;
  size_t overlap_ = kDefaultOverlap;

  BoundInput<TextBatch> in_text_;
  BoundOutput<TextBatch> out_chunks_;
  BoundOutput<Int32Batch> out_chunk_counts_;
};

NodeDefinition MakeTextChunkNodeDefinition() {
  NodeDefinition def;
  def.node_type = TextChunkNode::kNodeType;
  def.category = "common";
  def.validate_config = [](const nlohmann::json& config, const auto&,
                           std::string* diagnostic) {
    if (!ValidChunkConfig(config)) {
      if (diagnostic) *diagnostic = "overlap must be smaller than chunk_size";
      return false;
    }
    return true;
  };
  def.description =
      "UTF-8 code-point-safe text chunking with overlap and provenance";
  def.inputs = {RequiredInputPort("text",
                                  BlackboardKey<TextBatch>{"", "TextBatch"},
                                  "1:1", "preserve", "request")};
  def.outputs = {
      OutputPort("chunks", BlackboardKey<TextBatch>{"", "TextBatch"}, "1:N",
                 "generate_sub_id", "request"),
      OutputPort("chunk_counts", BlackboardKey<Int32Batch>{"", "Int32Batch"},
                 "1:1", "preserve", "request")};
  def.config_fields = TextChunkConfigFields();
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(TextChunkNode, MakeTextChunkNodeDefinition());

}  // namespace llm_edgeflow

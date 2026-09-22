#include <algorithm>
#include <string>
#include <vector>

#include "engine/text/utf8.h"
#include "nodes/authoring.h"
#include "nodes/node_error_codes.h"

namespace llm_edgeflow {
namespace {
struct ChunkInputs {
  const TextBatch* text = nullptr;
};
struct ChunkParams {
  int64_t chunk_size{};
  int64_t overlap{};
};
struct ChunkOutputs {
  TextBatch chunks;
  Int32Batch counts;
};

NodeResult<std::vector<std::string>> SplitText(const std::string& str,
                                               size_t chunk_size,
                                               size_t overlap) {
  if (str.empty()) {
    return NodeResult<std::vector<std::string>>::Success({""});
  }
  std::vector<size_t> boundaries;
  size_t invalid_offset = 0;
  if (!utf8::BuildCodePointBoundaries(str, &boundaries, &invalid_offset)) {
    return NodeResult<std::vector<std::string>>::Failure(
        NodeErrorKind::kBusinessError,
        "TextChunkNode invalid UTF-8 input at byte offset " +
            std::to_string(invalid_offset),
        node_error::text_chunk::kInvalidUtf8);
  }

  const size_t code_point_count = boundaries.size() - 1;
  const size_t step =
      (chunk_size > overlap) ? (chunk_size - overlap) : chunk_size;
  std::vector<std::string> chunks;
  for (size_t pos = 0; pos < code_point_count; pos += step) {
    const size_t end = std::min(pos + chunk_size, code_point_count);
    chunks.push_back(
        str.substr(boundaries[pos], boundaries[end] - boundaries[pos]));
    if (end == code_point_count) break;
  }
  return NodeResult<std::vector<std::string>>::Success(std::move(chunks));
}

NodeResult<ChunkOutputs> ChunkText(const ChunkInputs& inputs,
                                   const ChunkParams& params) {
  auto result = SplitPayloads(*inputs.text, [&](const std::string& text) {
    return SplitText(text, static_cast<size_t>(params.chunk_size),
                     static_cast<size_t>(params.overlap));
  });
  if (!result.ok()) return NodeResult<ChunkOutputs>::Failure(result.failure());
  auto split = std::move(result).value();
  return NodeResult<ChunkOutputs>::Success(
      {std::move(split.children), std::move(split.counts)});
}

auto TextChunkSpec() {
  auto params = Parameters<ChunkParams>(
      {Field("chunk_size", &ChunkParams::chunk_size)
           .Default(100)
           .Range(1, 1000000)
           .Description("每块最多包含的 Unicode 码点数；按 UTF-8 "
                        "字符边界切分，不是字节数或模型 token 数。"),
       Field("overlap", &ChunkParams::overlap)
           .Default(0)
           .Range(0, 100000)
           .Description("相邻块重叠的 Unicode 码点数，必须小于 chunk_size；0 "
                        "表示无重叠。")});
  params.Validate([](const ChunkParams& value, std::string* diagnostic) {
    if (value.overlap < value.chunk_size) return true;
    if (diagnostic) *diagnostic = "overlap must be smaller than chunk_size";
    return false;
  });
  return MakeBatchSpec(
             InputsOf<ChunkInputs>({Required("text", &ChunkInputs::text)}),
             OutputsOf<ChunkOutputs>(
                 {Produced("chunks", &ChunkOutputs::chunks,
                           PortFlow{"1:N", "generate_sub_id", "request"}),
                  Produced("chunk_counts", &ChunkOutputs::counts, "text")}),
             std::move(params), &ChunkText)
      .Category("common")
      .Description(
          "UTF-8 code-point-safe text chunking with overlap and provenance")
      .ParallelSafe(true);
}
}  // namespace
REGISTER_FUNCTION_NODE(TextChunkNode, TextChunkSpec());
}  // namespace llm_edgeflow

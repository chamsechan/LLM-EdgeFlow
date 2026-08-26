#pragma once

#include <string>
#include <vector>

#include "business/doc_qa/doc_qa_dto.h"
#include "core/blackboard_key.h"
#include "core/common_contracts.h"
#include "core/traceable_item.h"

namespace alg_framework {

inline constexpr char kDocQaBusinessName[] = "smart_doc_qa_v1";
inline constexpr char kDocQaOnnxBusinessName[] =
    "smart_doc_qa_onnx_llamacpp_v1";
inline constexpr char kDocQaRerankBusinessName[] = "smart_doc_qa_rerank_llm_v1";

inline constexpr BlackboardKey<std::vector<std::string>> kRawDocs{
    "raw_docs", "vector<string>"};

inline constexpr BlackboardKey<std::vector<TraceableItem<std::string>>>
    kChunkedDocItems{"chunked_doc_items", "traceable<string>[]"};

inline constexpr BlackboardKey<std::vector<TraceableItem<std::string>>>
    kQueryItems{"query_items", "traceable<string>[]"};

inline constexpr BlackboardKey<std::vector<int>> kChunkCountsPerReq{
    "chunk_counts_per_req", "vector<int>"};

inline constexpr BlackboardKey<std::vector<TraceableItem<std::vector<float>>>>
    kChunkEmbeddings{"chunk_embeddings", "traceable<vector<float>>[]"};

inline constexpr BlackboardKey<std::vector<TraceableItem<std::vector<float>>>>
    kQueryEmbeddings{"query_embeddings", "traceable<vector<float>>[]"};

inline constexpr BlackboardKey<std::vector<TraceableItem<std::string>>>
    kMatchedTopChunks{"matched_top_chunks", "traceable<string>[]"};

inline constexpr BlackboardKey<std::vector<std::string>> kRecognizedIntents{
    "recognized_intents", "vector<string>"};

inline constexpr BlackboardKey<std::vector<float>> kIntentConfidences{
    "intent_confidences", "vector<float>"};

inline constexpr BlackboardKey<std::vector<DocQaResult>> kFinalDocOutputs{
    "final_doc_outputs", "vector<DocQaResult>"};

}  // namespace alg_framework

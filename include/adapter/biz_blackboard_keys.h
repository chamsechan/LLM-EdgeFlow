#pragma once

#include <cstdint>
#include <vector>

#include "core/common_contracts.h"

namespace llm_edgeflow {

// 业务请求/响应槽位归接入适配层所有；
// 下层只消费逻辑端口绑定和中性值契约。
inline constexpr auto kInputSentences =
    MakeBlackboardKey<TextBatch>("input_sentences");
inline constexpr auto kRuleMatches =
    MakeBlackboardKey<RuleMatchBatch>("rule_matches");
inline constexpr auto kExtractedEntities =
    MakeBlackboardKey<StructuredDocumentBatch>("extracted_entities");
inline constexpr auto kRawDocs = MakeBlackboardKey<TextBatch>("raw_docs");
inline constexpr auto kRawQueries = MakeBlackboardKey<TextBatch>("raw_queries");
inline constexpr auto kLlmAnswers = MakeBlackboardKey<TextBatch>("llm_answers");
inline constexpr auto kIntentMatches =
    MakeBlackboardKey<RuleMatchBatch>("intent_matches");
inline constexpr auto kDocChunks = MakeBlackboardKey<TextBatch>("doc_chunks");
inline constexpr auto kDocChunkCounts =
    MakeBlackboardKey<Int32Batch>("doc_chunk_counts");
inline constexpr auto kUserTexts = MakeBlackboardKey<TextBatch>("user_texts");
inline constexpr auto kChannelNames =
    MakeBlackboardKey<TextBatch>("channel_names");
inline constexpr auto kStructuredVerdicts =
    MakeBlackboardKey<StructuredDocumentBatch>("structured_verdicts");
inline constexpr auto kMatchedPolicy =
    MakeBlackboardKey<RankedTextBatch>("matched_policy");
inline constexpr auto kImagePaths =
    MakeBlackboardKey<ImageRefBatch>("image_paths");
inline constexpr auto kUserQueries =
    MakeBlackboardKey<TextBatch>("user_queries");
inline constexpr auto kExtractedInvoiceJson =
    MakeBlackboardKey<StructuredDocumentBatch>("extracted_invoice_json");
inline constexpr auto kOcrDocs =
    MakeBlackboardKey<OcrDocumentBatch>("ocr_docs");
inline constexpr auto kAudioInputs =
    MakeBlackboardKey<AudioPcmBatch>("audio_inputs");
inline constexpr auto kIntentSlots =
    MakeBlackboardKey<RuleMatchBatch>("intent_slots");
inline constexpr auto kTranscripts =
    MakeBlackboardKey<TextBatch>("transcripts");
inline constexpr auto kRerankQueries =
    MakeBlackboardKey<TextBatch>("rerank_queries");
inline constexpr auto kRerankCandidates =
    MakeBlackboardKey<RankedTextBatch>("rerank_candidates");
inline constexpr auto kRerankPairs =
    MakeBlackboardKey<QueryCandidatesBatch>("rerank_pairs");
inline constexpr auto kRankedResults =
    MakeBlackboardKey<RankedTextBatch>("ranked_results");
inline constexpr auto kLlmInputPrompts =
    MakeBlackboardKey<TextBatch>("llm_input_prompts");
inline constexpr auto kGeneratedLlmAnswers =
    MakeBlackboardKey<TextBatch>("generated_llm_answers");

}  // namespace llm_edgeflow

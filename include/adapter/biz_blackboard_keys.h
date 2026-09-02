#pragma once

#include <cstdint>
#include <vector>

#include "core/common_contracts.h"

namespace llm_edgeflow {

// Business-facing request and response slots belong to the Layer 1 adapters.
// Lower layers consume logical port bindings and neutral value contracts only.
inline constexpr BlackboardKey<std::vector<uint64_t>> kRawRequestIds{
    "raw_request_ids", "vector<uint64>"};

inline constexpr BlackboardKey<TextBatch> kInputSentences{"input_sentences",
                                                          "TextBatch"};
inline constexpr BlackboardKey<RuleMatchBatch> kRuleMatches{"rule_matches",
                                                            "RuleMatchBatch"};
inline constexpr BlackboardKey<StructuredDocumentBatch> kExtractedEntities{
    "extracted_entities", "StructuredDocumentBatch"};
inline constexpr BlackboardKey<TextBatch> kRawDocs{"raw_docs", "TextBatch"};
inline constexpr BlackboardKey<TextBatch> kRawQueries{"raw_queries",
                                                      "TextBatch"};
inline constexpr BlackboardKey<TextBatch> kLlmAnswers{"llm_answers",
                                                      "TextBatch"};
inline constexpr BlackboardKey<RuleMatchBatch> kIntentMatches{"intent_matches",
                                                              "RuleMatchBatch"};
inline constexpr BlackboardKey<TextBatch> kDocChunks{"doc_chunks", "TextBatch"};
inline constexpr BlackboardKey<Int32Batch> kDocChunkCounts{"doc_chunk_counts",
                                                           "Int32Batch"};
inline constexpr BlackboardKey<TextBatch> kUserTexts{"user_texts", "TextBatch"};
inline constexpr BlackboardKey<TextBatch> kChannelNames{"channel_names",
                                                        "TextBatch"};
inline constexpr BlackboardKey<StructuredDocumentBatch> kStructuredVerdicts{
    "structured_verdicts", "StructuredDocumentBatch"};
inline constexpr BlackboardKey<RankedTextBatch> kMatchedPolicy{
    "matched_policy", "RankedTextBatch"};
inline constexpr BlackboardKey<ImageRefBatch> kImagePaths{"image_paths",
                                                          "ImageRefBatch"};
inline constexpr BlackboardKey<TextBatch> kUserQueries{"user_queries",
                                                       "TextBatch"};
inline constexpr BlackboardKey<StructuredDocumentBatch> kExtractedInvoiceJson{
    "extracted_invoice_json", "StructuredDocumentBatch"};
inline constexpr BlackboardKey<OcrDocumentBatch> kOcrDocs{"ocr_docs",
                                                          "OcrDocumentBatch"};
inline constexpr BlackboardKey<AudioPcmBatch> kAudioInputs{"audio_inputs",
                                                           "AudioPcmBatch"};
inline constexpr BlackboardKey<RuleMatchBatch> kIntentSlots{"intent_slots",
                                                            "RuleMatchBatch"};
inline constexpr BlackboardKey<TextBatch> kTranscripts{"transcripts",
                                                       "TextBatch"};
inline constexpr BlackboardKey<TextBatch> kRerankQueries{"rerank_queries",
                                                         "TextBatch"};
inline constexpr BlackboardKey<RankedTextBatch> kRerankCandidates{
    "rerank_candidates", "RankedTextBatch"};
inline constexpr BlackboardKey<QueryCandidatesBatch> kRerankPairs{
    "rerank_pairs", "QueryCandidatesBatch"};
inline constexpr BlackboardKey<RankedTextBatch> kRankedResults{
    "ranked_results", "RankedTextBatch"};
inline constexpr BlackboardKey<TextBatch> kLlmInputPrompts{"llm_input_prompts",
                                                           "TextBatch"};
inline constexpr BlackboardKey<TextBatch> kGeneratedLlmAnswers{
    "generated_llm_answers", "TextBatch"};

}  // namespace llm_edgeflow

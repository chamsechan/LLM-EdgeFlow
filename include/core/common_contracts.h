#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/blackboard_key.h"
#include "core/traceable_item.h"

namespace alg_framework {

// ==============================================================================
// 1. 标准控制命令常量 (Standard Named Control Commands)
// ==============================================================================

inline constexpr int kControlCmdUpdateRules = 1;
inline constexpr int kControlCmdUpdatePrompt = 2;
inline constexpr int kControlCmdUpdateThreshold = 3;
inline constexpr int kControlCmdUpdateParams = 4;

// ==============================================================================
// 2. 基础批处理值类型 (Common Contract Value Types)
// ==============================================================================

/**
 * @brief 标准可溯源文本批次 (TextBatch)
 */
using TextBatch = std::vector<TraceableItem<std::string>>;

/**
 * @brief 文本命名标量属性批次 (TextAttributesBatch)
 */
using TextAttributesBatch =
    std::vector<TraceableItem<std::unordered_map<std::string, std::string>>>;

/**
 * @brief 标准可溯源浮点特征向量批次 (EmbeddingBatch)
 */
using EmbeddingBatch = std::vector<TraceableItem<std::vector<float>>>;

/**
 * @brief 带打分与排名的排序候选载荷 (RankedCandidate)
 */
struct RankedCandidate {
  std::string text;
  float score = 0.0f;
  int rank = 0;
  uint32_t original_sub_id = 0;
  nlohmann::json metadata = nlohmann::json::object();

  RankedCandidate() = default;
  RankedCandidate(std::string t, float s = 0.0f, int r = 0,
                  uint32_t orig_sub = 0,
                  nlohmann::json meta = nlohmann::json::object())
      : text(std::move(t)),
        score(s),
        rank(r),
        original_sub_id(orig_sub),
        metadata(std::move(meta)) {}
};

/**
 * @brief 标准可溯源重排/排序候选批次 (RankedTextBatch)
 */
using RankedTextBatch = std::vector<TraceableItem<RankedCandidate>>;

/**
 * @brief 规则与关键词匹配结果载荷 (RuleMatchItem)
 */
struct RuleMatchItem {
  int is_hit = 0;
  int status_code = 0;
  std::string category;
  std::string rule_id;
  std::string matched_word;
  float score = 0.0f;
  std::unordered_map<std::string, std::string> captures;
  std::unordered_map<std::string, std::string> constants;
  std::string match_result_json;
  nlohmann::json details = nlohmann::json::object();

  RuleMatchItem() = default;
  RuleMatchItem(int hit, std::string cat, std::string word,
                std::string res_json = {}, float sc = 0.0f,
                std::string rid = {})
      : is_hit(hit),
        status_code(0),
        category(std::move(cat)),
        rule_id(std::move(rid)),
        matched_word(std::move(word)),
        score(sc),
        match_result_json(std::move(res_json)) {}
};

/**
 * @brief 标准可溯源规则匹配批次 (RuleMatchBatch)
 */
using RuleMatchBatch = std::vector<TraceableItem<RuleMatchItem>>;

/**
 * @brief 结构化文档解析状态枚举
 */
enum class JsonParseStatus {
  kOk = 0,
  kExtractedFromMarkdown = 1,
  kAutoClosed = 2,
  kFallbackApplied = 3,
  kFailed = 4,
};

/**
 * @brief 结构化文档/JSON 解析单项载荷 (JsonDocumentItem)
 */
/**
 * @brief 结构化文档/JSON 解析单项载荷 (JsonDocumentItem)
 */
struct JsonDocumentItem {
  std::string json_payload;
  nlohmann::json structured_data = nlohmann::json::object();
  bool is_valid = false;
  JsonParseStatus parse_status = JsonParseStatus::kOk;
  std::string diagnostic;

  JsonDocumentItem() = default;
  JsonDocumentItem(std::string payload, bool valid = true,
                   JsonParseStatus status = JsonParseStatus::kOk,
                   std::string diag = {},
                   nlohmann::json structured = nlohmann::json::object())
      : json_payload(std::move(payload)),
        structured_data(std::move(structured)),
        is_valid(valid),
        parse_status(status),
        diagnostic(std::move(diag)) {}

  const char* c_str() const { return json_payload.c_str(); }
  const std::string& str() const { return json_payload; }

  // 兼容直接字符串转换
  operator const std::string&() const { return json_payload; }
};

/**
 * @brief 标准可溯源结构化文档/JSON 批次 (JsonDocumentBatch /
 * StructuredDocumentBatch)
 */
using JsonDocumentBatch = std::vector<TraceableItem<JsonDocumentItem>>;
using StructuredDocumentBatch = JsonDocumentBatch;

/**
 * @brief 音频 PCM 浮点时序载荷 (AudioPcmPayload)
 */
struct AudioPcmPayload {
  std::vector<float> pcm_data;
  int sample_rate = 16000;

  AudioPcmPayload() = default;
  AudioPcmPayload(std::vector<float> data, int rate = 16000)
      : pcm_data(std::move(data)), sample_rate(rate) {}
};

/**
 * @brief 标准可溯源音频批次 (AudioPcmBatch)
 */
using AudioPcmBatch = std::vector<TraceableItem<AudioPcmPayload>>;

/**
 * @brief 图像文件路径或引用批次 (ImageRefBatch) - 具有强类型特质
 */
struct ImageRefBatch : public std::vector<TraceableItem<std::string>> {
  using std::vector<TraceableItem<std::string>>::vector;
  ImageRefBatch() = default;
  ImageRefBatch(std::vector<TraceableItem<std::string>> v)
      : std::vector<TraceableItem<std::string>>(std::move(v)) {}
};

/**
 * @brief OCR 矩形边界与文本识别框 (OcrBoxRecord)
 */
struct OcrBoxRecord {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  std::string text;
  float confidence = 0.0f;
};

/**
 * @brief OCR 文档识别结果项 (OcrDocumentItem)
 */
struct OcrDocumentItem {
  std::vector<OcrBoxRecord> boxes;
  std::string combined_text;
};

/**
 * @brief 标准可溯源 OCR 识别文档批次 (OcrDocumentBatch)
 */
using OcrDocumentBatch = std::vector<TraceableItem<OcrDocumentItem>>;

/**
 * @brief 查询-候选样本对载荷 (QueryCandidatePair)
 */
struct QueryCandidatePair {
  std::string query;
  std::string candidate;

  QueryCandidatePair() = default;
  QueryCandidatePair(std::string q, std::string c)
      : query(std::move(q)), candidate(std::move(c)) {}
};

/**
 * @brief 标准可溯源精排样本对批次 (QueryCandidatesBatch)
 */
using QueryCandidatesBatch = std::vector<TraceableItem<QueryCandidatePair>>;

// ==============================================================================
// 3. 编译期类型萃取特化 (BlackboardTypeTraits Specializations)
// ==============================================================================

template <>
struct BlackboardTypeTraits<TextBatch> {
  static constexpr const char* TypeName() { return "TextBatch"; }
};

template <>
struct BlackboardTypeTraits<TextAttributesBatch> {
  static constexpr const char* TypeName() { return "TextAttributesBatch"; }
};

template <>
struct BlackboardTypeTraits<EmbeddingBatch> {
  static constexpr const char* TypeName() { return "EmbeddingBatch"; }
};

template <>
struct BlackboardTypeTraits<RankedTextBatch> {
  static constexpr const char* TypeName() { return "RankedTextBatch"; }
};

template <>
struct BlackboardTypeTraits<RuleMatchBatch> {
  static constexpr const char* TypeName() { return "RuleMatchBatch"; }
};

template <>
struct BlackboardTypeTraits<JsonDocumentBatch> {
  static constexpr const char* TypeName() { return "StructuredDocumentBatch"; }
};

template <>
struct BlackboardTypeTraits<AudioPcmBatch> {
  static constexpr const char* TypeName() { return "AudioPcmBatch"; }
};

template <>
struct BlackboardTypeTraits<OcrDocumentBatch> {
  static constexpr const char* TypeName() { return "OcrDocumentBatch"; }
};

template <>
struct BlackboardTypeTraits<QueryCandidatesBatch> {
  static constexpr const char* TypeName() { return "QueryCandidatesBatch"; }
};

template <>
struct BlackboardTypeTraits<ImageRefBatch> {
  static constexpr const char* TypeName() { return "ImageRefBatch"; }
};

template <>
struct BlackboardTypeTraits<std::vector<uint64_t>> {
  static constexpr const char* TypeName() { return "vector<uint64>"; }
};

using Int32Batch = std::vector<TraceableItem<int32_t>>;

template <>
struct BlackboardTypeTraits<Int32Batch> {
  static constexpr const char* TypeName() { return "Int32Batch"; }
};

// ==============================================================================
// 4. 标准类型标识符与通用 BlackboardKey (Standard Keys)
// ==============================================================================

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

}  // namespace alg_framework

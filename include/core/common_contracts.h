#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "contracts/inference_payloads.h"
#include "contracts/traceable_item.h"
#include "core/blackboard_key.h"

namespace llm_edgeflow {

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
 * @brief 文本命名标量属性批次 (TextAttributesBatch)
 */
using TextAttributesBatch =
    std::vector<TraceableItem<std::unordered_map<std::string, std::string>>>;

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
};

/**
 * @brief 标准可溯源结构化文档/JSON 批次
 */
using StructuredDocumentBatch = std::vector<TraceableItem<JsonDocumentItem>>;

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
struct BlackboardTypeTraits<StructuredDocumentBatch> {
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

}  // namespace llm_edgeflow

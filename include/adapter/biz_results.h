#pragma once

#include <cstdint>
#include <string>

namespace llm_edgeflow {
// Layer 1 owned results. Strings have no C ABI array limit; the configured
// Operator output pool remains the authoritative external capacity boundary.

struct AuditResult {
  inline static constexpr char kTypeName[] = "AuditResult";
  uint64_t request_id{};
  std::string risk_level;
  float risk_score{};
  std::string matched_policy_clause;
  std::string audit_verdict_json;
  int status_code{};
};

struct KeywordResult {
  inline static constexpr char kTypeName[] = "KeywordResult";
  uint64_t request_id{};
  int is_hit{};
  std::string match_result_json;
  int status_code{};
};

struct EntityResult {
  inline static constexpr char kTypeName[] = "EntityResult";
  uint64_t request_id{};
  std::string entities_json;
  int status_code{};
};

struct DocResult {
  inline static constexpr char kTypeName[] = "DocResult";
  uint64_t request_id{};
  std::string intent_name;
  float confidence{};
  std::string answer_text;
  int chunk_count{};
  int status_code{};
};

struct OcrDocResult {
  inline static constexpr char kTypeName[] = "OcrDocResult";
  uint64_t request_id{};
  int detected_box_count{};
  std::string extracted_invoice_json;
  int status_code{};
};

struct AudioResult {
  inline static constexpr char kTypeName[] = "AudioResult";
  uint64_t request_id{};
  std::string transcribed_text;
  std::string intent_slot_json;
  int status_code{};
};

struct RerankResult {
  inline static constexpr char kTypeName[] = "RerankResult";
  uint64_t request_id{};
  float scores[8]{};
  int sorted_indices[8]{};
  int count{};
  int status_code{};
};

}  // namespace llm_edgeflow

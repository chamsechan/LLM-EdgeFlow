#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace llm_edgeflow {
// Request-owned values at the platform binding / Converter boundary.
// These describe content, never platform pointers, lengths or allocation.
struct TextInputValue {
  std::string sentence_text;
};
struct ImageInputValue {
  std::string image_uri;
};
struct DocumentInputValue {
  std::string doc_text;
  std::string query_text;
};
struct AuditInputValue {
  std::string user_text;
  std::string channel_name;
};
struct AudioInputValue {
  std::vector<float> pcm;
  int32_t sample_rate = 0;
};
struct RerankInputValue {
  std::string query_text;
  std::vector<std::string> candidate_passages;
};
struct EntityOutputValue {
  std::string entities_json;
  int32_t status_code = 0;
};
struct KeywordOutputValue {
  int32_t is_hit = 0;
  std::string match_result_json;
  int32_t status_code = 0;
};
struct DocumentOutputValue {
  std::string intent_name;
  float confidence = 0;
  std::string answer_text;
  int32_t chunk_count = 0;
  int32_t status_code = 0;
};
struct AuditOutputValue {
  std::string risk_level;
  float risk_score = 0;
  std::string matched_policy_clause;
  std::string audit_verdict_json;
  int32_t status_code = 0;
};
struct AudioOutputValue {
  std::string transcribed_text;
  std::string intent_slot_json;
  int32_t status_code = 0;
};
struct DetectionOutputValue {
  int32_t detected_box_count = 0;
  std::string result_json;
  int32_t status_code = 0;
};
struct RankedOutputItem {
  float score = 0;
  int32_t original_index = 0;
};
struct RerankOutputValue {
  std::vector<RankedOutputItem> items;
  int32_t status_code = 0;
};
}  // namespace llm_edgeflow

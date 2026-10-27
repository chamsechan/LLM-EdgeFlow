#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "contracts/inference_payloads.h"

namespace llm_edgeflow {
// 平台 binding 与 Converter 边界上由请求持有的值。
// 只描述内容，从不包含平台指针、长度或分配信息。
struct TextInputValue {
  std::string sentence_text;
};
struct ImageInputValue {
  ImageFrame frame;
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
struct JsonOutputValue {
  std::string result_json;
};
}  // namespace llm_edgeflow

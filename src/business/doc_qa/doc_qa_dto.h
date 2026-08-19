#pragma once

#include <cstdint>
#include <string>

namespace alg_framework {

/**
 * @brief 文档问答业务结果领域 DTO (Layer 3 <-> Layer 1 解耦)
 */
struct DocQaResult {
  uint64_t request_id = 0;
  std::string intent_name;
  float confidence = 0.0f;
  std::string answer_text;
  int chunk_count = 0;
  int status_code = 0;
};

}  // namespace alg_framework

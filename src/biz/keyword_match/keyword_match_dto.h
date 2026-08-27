#pragma once

#include <cstdint>
#include <string>

namespace alg_framework {

/**
 * @brief 关注词匹配业务结果领域 DTO (Layer 3 <-> Layer 1 解耦)
 */
struct KeywordMatchResult {
  uint64_t request_id = 0;
  int is_hit = 0;
  std::string match_result_json;
  int status_code = 0;
};

}  // namespace alg_framework

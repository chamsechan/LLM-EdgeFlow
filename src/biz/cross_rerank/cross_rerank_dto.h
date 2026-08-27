#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace alg_framework {

/**
 * @brief 语义精排输入领域 DTO (Layer 3 <-> Layer 1 解耦)
 */
struct RerankQueryInput {
  uint64_t request_id = 0;
  std::string query_text;
  std::vector<std::string> candidate_passages;
};

/**
 * @brief 语义精排结果领域 DTO (Layer 3 <-> Layer 1 解耦)
 */
struct RerankQueryResult {
  uint64_t request_id = 0;
  std::vector<float> scores;
  std::vector<int> sorted_indices;
  int count = 0;
  int status_code = 0;
};

}  // namespace alg_framework

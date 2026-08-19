#pragma once

#include <cstdint>
#include <string>

namespace alg_framework {

/**
 * @brief 实体提取业务结果领域 DTO (Layer 3 <-> Layer 1 解耦)
 */
struct EntityExtractResult {
  uint64_t request_id = 0;
  std::string entities_json;
  int status_code = 0;
};

}  // namespace alg_framework

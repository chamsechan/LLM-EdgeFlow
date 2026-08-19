#pragma once

#include <cstdint>
#include <string>

namespace alg_framework {

/**
 * @brief 语音识别与意图槽位抽取业务结果领域 DTO (Layer 3 <-> Layer 1 解耦)
 */
struct AudioAsrResult {
  uint64_t request_id = 0;
  std::string transcribed_text;
  std::string intent_slot_json;
  int status_code = 0;
};

}  // namespace alg_framework

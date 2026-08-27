#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace alg_framework {

/**
 * @brief 语音输入领域 DTO (Layer 1 <-> Layer 3 解耦)
 */
struct AudioInputDto {
  uint64_t request_id = 0;
  std::vector<float> pcm_data;
  int sample_rate = 16000;
};

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

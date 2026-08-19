#pragma once

#include <cstdint>
#include <string>

namespace alg_framework {

/**
 * @brief OCR票据文档问答业务结果领域 DTO (Layer 3 <-> Layer 1 解耦)
 */
struct OcrDocResult {
  uint64_t request_id = 0;
  int detected_box_count = 0;
  std::string extracted_invoice_json;
  int status_code = 0;
};

}  // namespace alg_framework

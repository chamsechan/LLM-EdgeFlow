#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_results.h"
#include "edgeflow/operator/types.h"
#include "platform_mock/error_codes.h"
#include "platform_mock/operator_data_types.h"

namespace llm_edgeflow {

/**
 * @brief Owned 外部文本请求 (ADP-002, RFC-0053)
 *
 * 封装已校验并深拷贝的外部 request_id 与文本内容。
 */
struct OwnedTextRequest {
  uint64_t request_id{0};
  std::string text;

  OwnedTextRequest() = default;
  OwnedTextRequest(uint64_t id, std::string t)
      : request_id(id), text(std::move(t)) {}
};

inline constexpr size_t kMaxTextCarrierSentenceLen = 64 * 1024;  // 64KB

/**
 * @brief 文本载体输入批次校验与完整 copy-in
 *
 * 必须在逐项调用业务 Decode
 * 之前完成整批载体校验，以保证跨样本载体/业务错误优先级。
 */
inline int UnpackTextCarrierBatch(const void** inputs, int num_inputs,
                                  int max_batch_size, const char* adapter_name,
                                  std::vector<OwnedTextRequest>* out_requests,
                                  AdapterStatus* out_status = nullptr) {
  int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
      inputs, num_inputs, max_batch_size, adapter_name);
  if (valid_ret != 0) {
    return AdapterValidationHelper::ReturnInvalidInput(
        out_status, "Batch envelope validation failed or null AlgContext",
        "inputs", adapter_name);
  }
  if (!out_requests) {
    return AdapterValidationHelper::ReturnInvalidInput(
        out_status, "Null out_requests destination pointer", "inputs",
        adapter_name);
  }
  out_requests->clear();
  out_requests->reserve(num_inputs);
  for (int i = 0; i < num_inputs; ++i) {
    const auto* in = static_cast<const CompanyOperatorEntityInput*>(inputs[i]);
    if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in, i,
                                                 adapter_name, out_status)) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    if (!in->sentence_text || in->sentence_text->length < 0 ||
        (in->sentence_text->length > 0 && !in->sentence_text->data)) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    if (static_cast<size_t>(in->sentence_text->length) >
        kMaxTextCarrierSentenceLen) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    out_requests->emplace_back(
        in->request_id,
        std::string(in->sentence_text->data, in->sentence_text->length));
  }
  return COMPANY_ALG_SUCCESS;
}
}  // namespace llm_edgeflow

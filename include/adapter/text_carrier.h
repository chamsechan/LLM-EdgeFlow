#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_results.h"
#include "adapter/operator_biz_bridge.h"
#include "edgeflow/c_api.h"
#include "edgeflow/operator/types.h"

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
    const auto* in = static_cast<const CompanyEntityInputStruct*>(inputs[i]);
    if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in, i,
                                                 adapter_name, out_status)) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    if (!AdapterValidationHelper::RequireBoundedString(
            "inputs[i].sentence_text", in->sentence_text,
            kMaxTextCarrierSentenceLen, i, adapter_name, out_status)) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    out_requests->emplace_back(in->request_id, std::string(in->sentence_text));
  }
  return COMPANY_ALG_SUCCESS;
}

/**
 * @brief 文本载体输出受检写入 (支持固定 C 结构体与 owned Result)
 */
template <typename Output>
inline int WriteTextCarrierOutput(Output* out, uint64_t request_id,
                                  int status_code, const std::string& text,
                                  int sample_idx, const char* adapter_name,
                                  AdapterStatus* out_status);

template <>
inline int WriteTextCarrierOutput<CompanyEntityOutputStruct>(
    CompanyEntityOutputStruct* out, uint64_t request_id, int status_code,
    const std::string& text, int sample_idx, const char* adapter_name,
    AdapterStatus* out_status) {
  if (!out) return COMPANY_ALG_ERR_INVALID_INPUT;
  out->request_id = request_id;
  out->status_code = status_code;
  if (!AdapterValidationHelper::CheckedStringCopy(
          out->entities_json, sizeof(out->entities_json), text.c_str(),
          "outputs[i].entities_json", sample_idx, adapter_name, out_status)) {
    return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
  }
  return COMPANY_ALG_SUCCESS;
}

template <>
inline int WriteTextCarrierOutput<EntityResult>(
    EntityResult* out, uint64_t request_id, int status_code,
    const std::string& text, int sample_idx, const char* adapter_name,
    AdapterStatus* out_status) {
  (void)sample_idx;
  (void)adapter_name;
  (void)out_status;
  if (!out) return COMPANY_ALG_ERR_INVALID_INPUT;
  out->request_id = request_id;
  out->status_code = status_code;
  out->entities_json = text;
  return COMPANY_ALG_SUCCESS;
}

/**
 * @brief 共享文本载体 Operator 单槽位转换函数
 */
int ConvertTextCarrierInput(const CompanyOperatorEntityInput& in,
                            ProcessLocalShadowStorage& storage,
                            const CompanyEntityInputStruct** out_internal_dto,
                            std::string* err);

int ConvertTextCarrierOutput(const EntityResult& in_dto,
                             CompanyOperatorEntityOutput& out,
                             const ResolvedOutputPoolSpec& spec,
                             std::string* err);

/**
 * @brief 共享文本载体 Operator 桥接描述符构造
 */
OperatorBizBridgeDescriptor MakeTextCarrierBridge(CompanyAlgBizType biz_type,
                                                  std::string adapter_name,
                                                  std::string identity);

}  // namespace llm_edgeflow

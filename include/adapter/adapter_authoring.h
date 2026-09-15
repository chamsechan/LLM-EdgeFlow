#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "adapter/adapter_batch.h"
#include "adapter/adapter_result.h"
#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_adapter_interface.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/biz_results.h"
#include "adapter/result_packing_adapter.h"
#include "adapter/result_validation.h"
#include "adapter/text_carrier.h"
#include "core/alg_context.h"
#include "core/biz_definition.h"
#include "core/blackboard_key.h"
#include "edgeflow/c_api.h"

namespace llm_edgeflow {

/**
 * @brief 1:1 文本业务适配器声明规格 (ADP-001 ~ ADP-011, RFC-0053)
 *
 * 封装业务元数据、带类型端口声明、单样本 Decode/Encode 函数以及确定性执行顺序。
 */
struct OneToOneTextAdapterSpec {
  CompanyAlgBizType biz_type = ALG_BIZ_TYPE_UNKNOWN;
  const char* adapter_name = "";
  const char* sdk_abi_version = COMPANY_ALG_ABI_VERSION;
  const char* c_input_type_name = "CompanyEntityInputStruct";
  const char* c_output_type_name = "CompanyEntityOutputStruct";
  int max_batch_size = 64;
  OwnershipPolicy ownership_policy = OwnershipPolicy::kCopyIn;
  ThreadModel thread_model = ThreadModel::kStatelessThreadSafe;
  OutputCardinality cardinality = OutputCardinality::kOneToOne;

  const char* biz_name = "";
  const char* demo_biz = "";
  const char* display_name = "";

  BlackboardKey<TextBatch> input_key = kInputSentences;
  BlackboardKey<TextBatch> output_key = kLlmAnswers;

  using DecodeFn = AdapterResult<std::string> (*)(const OwnedTextRequest&);
  using EncodeFn = AdapterResult<std::string> (*)(const std::string&);

  DecodeFn decode_fn = nullptr;
  EncodeFn encode_fn = nullptr;

  const char* carrier_adapter_name = "EntityExtract";

  using NullContextHook = int (*)(const char* adapter_name,
                                  AdapterStatus* out_status);
  NullContextHook unpack_null_ctx_hook = nullptr;
  NullContextHook pack_null_ctx_hook = nullptr;
  const char* null_ctx_field = nullptr;

  AdapterDescriptor GetDescriptor() const {
    AdapterDescriptor desc;
    desc.biz_type = biz_type;
    desc.adapter_name = adapter_name ? adapter_name : "";
    desc.sdk_abi_version =
        sdk_abi_version ? sdk_abi_version : COMPANY_ALG_ABI_VERSION;
    desc.input_type_name = c_input_type_name ? c_input_type_name : "";
    desc.output_type_name = c_output_type_name ? c_output_type_name : "";
    desc.max_batch_size = max_batch_size;
    desc.ownership_policy = ownership_policy;
    desc.thread_model = thread_model;
    desc.cardinality = cardinality;
    desc.biz_definitions = {
        {biz_name ? biz_name : "",
         demo_biz ? demo_biz : "",
         display_name ? display_name : "",
         {RequiredBizInput(kRawRequestIds), RequiredBizInput(input_key)},
         {BizOutput(output_key)}}};
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx,
             AdapterStatus* out_status = nullptr) const {
    if (!ctx) {
      if (unpack_null_ctx_hook) {
        return unpack_null_ctx_hook(adapter_name, out_status);
      }
      if (null_ctx_field) {
        return AdapterValidationHelper::ReturnInvalidInput(
            out_status, "Missing text carrier or context", null_ctx_field,
            adapter_name);
      }
      return AdapterValidationHelper::ReturnInvalidInput(
          out_status, "Batch envelope validation failed or null AlgContext",
          "inputs", adapter_name);
    }
    if (!decode_fn) {
      return AdapterValidationHelper::ReturnInvalidInput(
          out_status, "Missing decode_fn in OneToOneTextAdapterSpec",
          "decode_fn", adapter_name);
    }
    return UnpackTextBatchSkeleton(inputs, num_inputs, max_batch_size,
                                   adapter_name, ctx, input_key, decode_fn,
                                   out_status, carrier_adapter_name);
  }

  template <typename Output>
  int PackTyped(AlgContext* ctx, void** outputs, int* num_outputs,
                AdapterStatus* out_status = nullptr) const {
    if (!ctx) {
      if (pack_null_ctx_hook) {
        return pack_null_ctx_hook(adapter_name, out_status);
      }
      if (null_ctx_field) {
        return AdapterValidationHelper::ReturnInvalidInput(
            out_status, "Missing text carrier or context", null_ctx_field,
            adapter_name);
      }
      return AdapterValidationHelper::ReturnBufferTooSmall(
          out_status, "Null AlgContext passed to Pack", "ctx", adapter_name);
    }

    const auto* answers = AdapterValidationHelper::ReadRequiredContextValue(
        *ctx, output_key, adapter_name, out_status);
    const auto* raw_req_ids = AdapterValidationHelper::ReadRequiredContextValue(
        *ctx, kRawRequestIds, adapter_name, out_status);
    if (!answers || !raw_req_ids) {
      // 兼容约定：底层 ReadRequiredContextValue 写入 BufferTooSmall 诊断，
      // 但 Translate 缺 answers/IDs 时返回码保持 INVALID_INPUT。
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }

    if (!encode_fn) {
      return AdapterValidationHelper::ReturnInvalidInput(
          out_status, "Missing encode_fn in OneToOneTextAdapterSpec",
          "encode_fn", adapter_name);
    }

    // PrepareResults: 业务转换阶段，全批序列化先于容量与来源校验
    std::vector<std::string> encoded_answers;
    encoded_answers.reserve(answers->size());
    for (const auto& item : *answers) {
      auto res = encode_fn(item.data);
      if (!res.IsOk()) {
        if (out_status) *out_status = res.Status();
        return res.ReturnCode();
      }
      encoded_answers.push_back(res.TakeValue());
    }

    int count = static_cast<int>(answers->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, carrier_adapter_name, out_status);
    if (valid_ret != 0) return valid_ret;

    std::vector<const TextBatch::value_type*> answers_by_request;
    if (!IndexResults(answers, raw_req_ids, &answers_by_request, "res",
                      carrier_adapter_name, out_status)) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<Output*>(outputs[i]);
      uint64_t req_id = (*raw_req_ids)[i];
      size_t answer_idx =
          static_cast<size_t>(answers_by_request[i] - answers->data());
      const std::string& json_text = encoded_answers[answer_idx];
      int write_ret = WriteTextCarrierOutput(out_ptr, req_id, 0, json_text, i,
                                             carrier_adapter_name, out_status);
      if (write_ret != COMPANY_ALG_SUCCESS) {
        return write_ret;
      }
    }

    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

/**
 * @brief 基于 Spec 声明生成的轻量 IBizAdapter 模板包装 (RFC-0053)
 */
template <typename SpecProvider>
class OneToOneTextAdapter
    : public ResultPackingAdapter<OneToOneTextAdapter<SpecProvider>,
                                  CompanyEntityOutputStruct, EntityResult> {
 public:
  static const OneToOneTextAdapterSpec& Spec() {
    return SpecProvider::GetSpec();
  }

  CompanyAlgBizType BizType() const override { return Spec().biz_type; }

  const char* AdapterName() const override { return Spec().adapter_name; }

  const AdapterDescriptor& GetDescriptor() const override {
    static const AdapterDescriptor desc = Spec().GetDescriptor();
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx,
             AdapterStatus* out_status = nullptr) const override {
    return Spec().Unpack(inputs, num_inputs, ctx, out_status);
  }

  template <typename Output>
  int PackTyped(AlgContext* ctx, void** outputs, int* count,
                AdapterStatus* status = nullptr) const {
    return Spec().template PackTyped<Output>(ctx, outputs, count, status);
  }
};

}  // namespace llm_edgeflow

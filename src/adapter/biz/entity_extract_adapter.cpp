#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_adapter_registry.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/biz_results.h"
#include "adapter/result_packing_adapter.h"
#include "adapter/result_validation.h"
#include "edgeflow/c_api.h"

namespace llm_edgeflow {

inline static constexpr char kEntityExtractBizName[] = "entity_extract_v1";

class EntityExtractAdapter
    : public ResultPackingAdapter<EntityExtractAdapter,
                                  CompanyEntityOutputStruct, EntityResult> {
 public:
  CompanyAlgBizType BizType() const override {
    return ALG_BIZ_TYPE_ENTITY_EXTRACT;
  }

  const char* AdapterName() const override { return "EntityExtract"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{
        ALG_BIZ_TYPE_ENTITY_EXTRACT,
        "EntityExtract",
        COMPANY_ALG_ABI_VERSION,
        "CompanyEntityInputStruct",
        "CompanyEntityOutputStruct",
        64,
        OwnershipPolicy::kCopyIn,
        ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne,
        {{kEntityExtractBizName,
          "entity_extract",
          "实体抽取",
          {RequiredBizInput(kRawRequestIds), RequiredBizInput(kInputSentences)},
          {BizOutput(kExtractedEntities)}}}};
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx,
             AdapterStatus* out_status = nullptr) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, AdapterName());
    if (valid_ret != 0 || !ctx) {
      return AdapterValidationHelper::ReturnInvalidInput(
          out_status, "Batch envelope validation failed or null AlgContext",
          "inputs", AdapterName());
    }

    std::vector<uint64_t> req_ids;
    TextBatch sentences;
    req_ids.reserve(num_inputs);
    sentences.reserve(num_inputs);

    constexpr size_t kMaxSentenceLen = 64 * 1024;  // 64KB 单文本上限

    for (int i = 0; i < num_inputs; ++i) {
      auto* in = static_cast<const CompanyEntityInputStruct*>(inputs[i]);
      if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in, i,
                                                   AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      if (!AdapterValidationHelper::RequireBoundedString(
              "inputs[i].sentence_text", in->sentence_text, kMaxSentenceLen, i,
              AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      req_ids.push_back(in->request_id);
      sentences.emplace_back(static_cast<uint32_t>(i), 0, in->sentence_text);
    }

    if (!AdapterValidationHelper::PublishContextValue(
            *ctx, kRawRequestIds, std::move(req_ids), AdapterName(),
            out_status) ||
        !AdapterValidationHelper::PublishContextValue(
            *ctx, kInputSentences, std::move(sentences), AdapterName(),
            out_status)) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    return COMPANY_ALG_SUCCESS;
  }

  template <typename Output>
  int PackTyped(AlgContext* ctx, void** outputs, int* num_outputs,
                AdapterStatus* out_status = nullptr) const {
    if (!ctx) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          out_status, "Null AlgContext passed to Pack", "ctx", AdapterName());
    }

    const auto* res = AdapterValidationHelper::ReadRequiredContextValue(
        *ctx, kExtractedEntities, AdapterName(), out_status);
    if (!res) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    const auto* raw_req_ids = ctx->Read(kRawRequestIds);

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, AdapterName(), out_status);
    if (valid_ret != 0) return valid_ret;

    std::vector<const StructuredDocumentBatch::value_type*> res_by_request;
    if (!IndexResults(res, raw_req_ids, &res_by_request, "res", AdapterName(),
                      out_status))
      return COMPANY_ALG_ERR_INVALID_INPUT;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<Output*>(outputs[i]);
      uint64_t req_id =
          (raw_req_ids && i < static_cast<int>(raw_req_ids->size()))
              ? (*raw_req_ids)[i]
              : res_by_request[i]->req_id;
      out_ptr->request_id = req_id;
      if (!IsSuccessfulDocument(res_by_request[i]->data)) {
        return AdapterValidationHelper::ReturnInvalidInput(
            out_status, "Structured result failed or used fallback", "res",
            AdapterName(), i);
      }
      out_ptr->status_code = 0;

      if (!CopyResultString(out_ptr->entities_json,
                            res_by_request[i]->data.json_payload.c_str(),
                            "outputs[i].entities_json", i, AdapterName(),
                            out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BIZ_ADAPTER(EntityExtractAdapter);

}  // namespace llm_edgeflow

#include <vector>

#include "adapter/adapter_batch.h"
#include "adapter/biz_adapter_registry.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/biz_results.h"
#include "adapter/result_packing_adapter.h"
#include "adapter/result_validation.h"
#include "adapter/text_carrier.h"
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
    return UnpackTextBatchSkeleton(
        inputs, num_inputs, GetDescriptor().max_batch_size, AdapterName(), ctx,
        kInputSentences,
        [](const OwnedTextRequest& req) {
          return AdapterResult<std::string>::Ok(req.text);
        },
        out_status);
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
      // 保持行为：逐行先写 request_id，再检查结构化状态
      out_ptr->request_id = (*raw_req_ids)[i];
      if (!IsSuccessfulDocument(res_by_request[i]->data)) {
        return AdapterValidationHelper::ReturnInvalidInput(
            out_status, "Structured result failed or used fallback", "res",
            AdapterName(), i);
      }
      out_ptr->status_code = 0;

      int write_ret = WriteTextCarrierOutput(
          out_ptr, (*raw_req_ids)[i], 0, res_by_request[i]->data.json_payload,
          i, AdapterName(), out_status);
      if (write_ret != COMPANY_ALG_SUCCESS) {
        return write_ret;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BIZ_ADAPTER(EntityExtractAdapter);

}  // namespace llm_edgeflow

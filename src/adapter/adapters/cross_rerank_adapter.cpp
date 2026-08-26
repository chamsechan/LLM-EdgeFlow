#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/business_adapter_registry.h"
#include "business/cross_rerank/cross_rerank_contract.h"
#include "company_alg_interface.h"

namespace alg_framework {

class CrossRerankAdapter : public IBusinessAdapter {
 public:
  CompanyAlgBizType BizType() const override {
    return ALG_BIZ_TYPE_CROSS_RERANK;
  }

  const char* BizName() const override { return "CrossRerank"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{ALG_BIZ_TYPE_CROSS_RERANK,
                                  "CrossRerank",
                                  "2.0.0",
                                  "CompanyRerankBatchInputStruct",
                                  "CompanyRerankBatchOutputStruct",
                                  64,
                                  OwnershipPolicy::kCopyIn,
                                  ThreadModel::kStatelessThreadSafe,
                                  OutputCardinality::kOneToOne,
                                  {{kCrossRerankBusinessName,
                                    "cross_rerank",
                                    "Cross-Encoder 精排",
                                    {RequiredInput(kRawRerankInputs)},
                                    {Output(kRerankBatchFinalOutputs)}}}};
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx,
             AdapterStatus* out_status = nullptr) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, BizName());
    if (valid_ret != 0 || !ctx) {
      if (out_status) {
        *out_status = AdapterStatus::InvalidInput(
            "Batch envelope validation failed or null AlgContext", "inputs", -1,
            BizName());
      }
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }

    std::vector<RerankQueryInput> raw_inputs;
    raw_inputs.reserve(num_inputs);

    constexpr size_t kMaxTextLen = 64 * 1024;  // 64KB 单文本上限

    for (int i = 0; i < num_inputs; ++i) {
      auto* in_rerank =
          static_cast<const CompanyRerankBatchInputStruct*>(inputs[i]);
      if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in_rerank, i,
                                                   BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      // ADP-001, RECHECK-004: 有界字符串强校验
      if (!AdapterValidationHelper::RequireBoundedCompanyString(
              "inputs[i].query_text", in_rerank->query_text, kMaxTextLen, i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      // ADP-001, ADP-005: 严格校验 candidate_count (1~8)，杜绝越界或静默截断
      if (!AdapterValidationHelper::RequireRange("inputs[i].candidate_count",
                                                 in_rerank->candidate_count, 1,
                                                 8, i, BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      RerankQueryInput query_item;
      query_item.request_id = in_rerank->request_id;
      query_item.query_text = in_rerank->query_text->data;

      for (int c = 0; c < in_rerank->candidate_count; ++c) {
        std::string field_name =
            "inputs[i].candidate_passages[" + std::to_string(c) + "]";
        if (!AdapterValidationHelper::RequireBoundedCompanyString(
                field_name.c_str(), in_rerank->candidate_passages[c],
                kMaxTextLen, i, BizName(), out_status)) {
          return COMPANY_ALG_ERR_INVALID_INPUT;
        }
        query_item.candidate_passages.push_back(
            in_rerank->candidate_passages[c]->data);
      }
      raw_inputs.push_back(std::move(query_item));
    }

    ctx->Set(kRawRerankInputs, std::move(raw_inputs));
    return COMPANY_ALG_SUCCESS;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs,
           AdapterStatus* out_status = nullptr) const override {
    if (!ctx) {
      if (out_status) {
        *out_status = AdapterStatus::BufferTooSmall(
            "Null AlgContext passed to Pack", "ctx", -1, BizName());
      }
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }

    auto* res = ctx->Get(kRerankBatchFinalOutputs);
    if (!res) {
      if (out_status) {
        *out_status = AdapterStatus::BufferTooSmall(
            "rerank_batch_final_outputs not found in AlgContext",
            "rerank_batch_final_outputs", -1, BizName());
      }
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName());
    if (valid_ret != 0) {
      if (out_status) {
        *out_status = AdapterStatus::BufferTooSmall(
            "Output slots insufficient or null", "outputs", -1, BizName());
      }
      return valid_ret;
    }

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<CompanyRerankBatchOutputStruct*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->count = (*res)[i].count;
      out_ptr->status_code = (*res)[i].status_code;

      for (int k = 0; k < (*res)[i].count && k < 8; ++k) {
        out_ptr->scores[k] = (*res)[i].scores[k];
        out_ptr->sorted_indices[k] = (*res)[i].sorted_indices[k];
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BUSINESS_ADAPTER(CrossRerankAdapter);

}  // namespace alg_framework

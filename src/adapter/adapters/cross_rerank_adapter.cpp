#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/business_adapter_registry.h"
#include "business/cross_rerank/cross_rerank_dto.h"
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
                                  OutputCardinality::kOneToOne};
    return desc;
  }

  bool ValidatePipelineBinding(
      const std::string& pipeline_biz_name) const override {
    return pipeline_biz_name.find("rerank") != std::string::npos ||
           pipeline_biz_name == "CrossRerank";
  }

  int Unpack(const void** inputs, int num_inputs,
             AlgContext* ctx) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, BizName());
    if (valid_ret != 0 || !ctx) return COMPANY_ALG_ERR_INVALID_INPUT;

    std::vector<RerankQueryInput> raw_inputs;
    raw_inputs.reserve(num_inputs);

    for (int i = 0; i < num_inputs; ++i) {
      auto* in_rerank =
          static_cast<const CompanyRerankBatchInputStruct*>(inputs[i]);
      if (!in_rerank || !in_rerank->query_text) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      // ADP-001, ADP-005: 严格校验 candidate_count (1~8)，杜绝越界或静默截断
      if (in_rerank->candidate_count <= 0 || in_rerank->candidate_count > 8) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      RerankQueryInput query_item;
      query_item.request_id = in_rerank->request_id;
      query_item.query_text = in_rerank->query_text;

      for (int c = 0; c < in_rerank->candidate_count; ++c) {
        if (!in_rerank->candidate_passages[c]) {
          return COMPANY_ALG_ERR_INVALID_INPUT;
        }
        query_item.candidate_passages.push_back(
            in_rerank->candidate_passages[c]);
      }
      raw_inputs.push_back(std::move(query_item));
    }

    ctx->Set("raw_rerank_inputs", std::move(raw_inputs));
    return COMPANY_ALG_SUCCESS;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs) const override {
    if (!ctx) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    auto* res =
        ctx->Get<std::vector<RerankQueryResult>>("rerank_batch_final_outputs");
    if (!res) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName());
    if (valid_ret != 0) return valid_ret;

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

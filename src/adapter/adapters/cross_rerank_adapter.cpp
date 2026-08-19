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
                                  64};
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx) override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, BizName());
    if (valid_ret != 0 || !ctx) return -3;

    std::vector<RerankQueryInput> raw_inputs;
    raw_inputs.reserve(num_inputs);

    for (int i = 0; i < num_inputs; ++i) {
      auto* in_rerank =
          static_cast<const CompanyRerankBatchInputStruct*>(inputs[i]);

      RerankQueryInput query_item;
      query_item.request_id = in_rerank->request_id;
      query_item.query_text =
          in_rerank->query_text ? in_rerank->query_text : "";

      for (int c = 0; c < in_rerank->candidate_count && c < 8; ++c) {
        query_item.candidate_passages.push_back(
            in_rerank->candidate_passages[c] ? in_rerank->candidate_passages[c]
                                             : "");
      }
      raw_inputs.push_back(std::move(query_item));
    }

    ctx->Set("raw_rerank_inputs", std::move(raw_inputs));
    return 0;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs) override {
    if (!ctx) return -4;

    auto* res =
        ctx->Get<std::vector<RerankQueryResult>>("rerank_batch_final_outputs");
    if (!res) return -4;

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
    return 0;
  }
};

REGISTER_BUSINESS_ADAPTER(CrossRerankAdapter);

}  // namespace alg_framework

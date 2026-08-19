#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/business_adapter_registry.h"
#include "business/doc_qa/doc_qa_dto.h"
#include "company_alg_interface.h"

namespace alg_framework {

class DocQaAdapter : public IBusinessAdapter {
 public:
  CompanyAlgBizType BizType() const override { return ALG_BIZ_TYPE_DOC_QA; }

  const char* BizName() const override { return "DocQA"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{ALG_BIZ_TYPE_DOC_QA,
                                  "DocQA",
                                  "2.0.0",
                                  "CompanyDocInputStruct",
                                  "CompanyDocOutputStruct",
                                  64,
                                  OwnershipPolicy::kCopyIn,
                                  ThreadModel::kStatelessThreadSafe,
                                  OutputCardinality::kOneToOne};
    return desc;
  }

  bool ValidatePipelineBinding(
      const std::string& pipeline_biz_name) const override {
    return pipeline_biz_name.find("doc_qa") != std::string::npos ||
           pipeline_biz_name == "DocQA";
  }

  int Unpack(const void** inputs, int num_inputs,
             AlgContext* ctx) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, BizName());
    if (valid_ret != 0 || !ctx) return COMPANY_ALG_ERR_INVALID_INPUT;

    std::vector<uint64_t> raw_req_ids;
    std::vector<std::string> raw_docs;
    std::vector<std::string> raw_queries;

    raw_req_ids.reserve(num_inputs);
    raw_docs.reserve(num_inputs);
    raw_queries.reserve(num_inputs);

    for (int i = 0; i < num_inputs; ++i) {
      auto* in_doc = static_cast<const CompanyDocInputStruct*>(inputs[i]);
      if (!in_doc || !in_doc->query_text) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      raw_req_ids.push_back(in_doc->request_id);
      raw_docs.push_back(in_doc->doc_text ? in_doc->doc_text : "");
      raw_queries.push_back(in_doc->query_text);
    }

    ctx->Set("raw_request_ids", std::move(raw_req_ids));
    ctx->Set("raw_docs", std::move(raw_docs));
    ctx->Set("raw_queries", std::move(raw_queries));
    return COMPANY_ALG_SUCCESS;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs) const override {
    if (!ctx) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    auto* res = ctx->Get<std::vector<DocQaResult>>("final_doc_outputs");
    if (!res) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName());
    if (valid_ret != 0) return valid_ret;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<CompanyDocOutputStruct*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->confidence = (*res)[i].confidence;
      out_ptr->chunk_count = (*res)[i].chunk_count;
      out_ptr->status_code = (*res)[i].status_code;

      AdapterValidationHelper::CheckedStringCopy(
          out_ptr->intent_name, sizeof(out_ptr->intent_name),
          (*res)[i].intent_name.c_str(), "outputs[i].intent_name", i,
          BizName());

      AdapterValidationHelper::CheckedStringCopy(
          out_ptr->answer_text, sizeof(out_ptr->answer_text),
          (*res)[i].answer_text.c_str(), "outputs[i].answer_text", i,
          BizName());
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BUSINESS_ADAPTER(DocQaAdapter);

}  // namespace alg_framework

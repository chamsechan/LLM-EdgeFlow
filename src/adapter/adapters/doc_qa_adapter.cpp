#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/business_adapter_registry.h"
#include "business/doc_qa/doc_qa_contract.h"
#include "company_alg_interface.h"

namespace alg_framework {

class DocQaAdapter : public IBusinessAdapter {
 public:
  CompanyAlgBizType BizType() const override { return ALG_BIZ_TYPE_DOC_QA; }

  const char* BizName() const override { return "DocQA"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{
        ALG_BIZ_TYPE_DOC_QA,
        "DocQA",
        "2.0.0",
        "CompanyDocInputStruct",
        "CompanyDocOutputStruct",
        64,
        OwnershipPolicy::kCopyIn,
        ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne,
        {{kDocQaBusinessName,
          "doc_qa",
          "智能文档问答",
          {RequiredInput(kRawRequestIds), RequiredInput(kRawDocs),
           RequiredInput(kRawQueries)},
          {Output(kFinalDocOutputs)}},
         {kDocQaOnnxBusinessName,
          "doc_qa",
          "智能文档问答（ONNX/llama.cpp）",
          {RequiredInput(kRawRequestIds), RequiredInput(kRawDocs),
           RequiredInput(kRawQueries)},
          {Output(kFinalDocOutputs)}},
         {kDocQaRerankBusinessName,
          "doc_qa",
          "智能文档问答（精排）",
          {RequiredInput(kRawRequestIds), RequiredInput(kRawDocs),
           RequiredInput(kRawQueries)},
          {Output(kFinalDocOutputs)}}}};
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

    std::vector<uint64_t> raw_req_ids;
    std::vector<std::string> raw_docs;
    std::vector<std::string> raw_queries;

    raw_req_ids.reserve(num_inputs);
    raw_docs.reserve(num_inputs);
    raw_queries.reserve(num_inputs);

    constexpr size_t kMaxQueryLen = 64 * 1024;       // 64KB
    constexpr size_t kMaxDocLen = 10 * 1024 * 1024;  // 10MB 单文档上限

    for (int i = 0; i < num_inputs; ++i) {
      auto* in_doc = static_cast<const CompanyDocInputStruct*>(inputs[i]);
      if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in_doc, i,
                                                   BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      // ADP-001, RECHECK-004: 有界字符串强校验
      if (!AdapterValidationHelper::RequireBoundedCompanyString(
              "inputs[i].query_text", in_doc->query_text, kMaxQueryLen, i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      if (in_doc->doc_text) {
        if (!AdapterValidationHelper::RequireBoundedCompanyString(
                "inputs[i].doc_text", in_doc->doc_text, kMaxDocLen, i,
                BizName(), out_status)) {
          return COMPANY_ALG_ERR_INVALID_INPUT;
        }
      }

      raw_req_ids.push_back(in_doc->request_id);
      raw_docs.push_back((in_doc->doc_text && in_doc->doc_text->data)
                             ? in_doc->doc_text->data
                             : "");
      raw_queries.push_back(in_doc->query_text->data);
    }

    ctx->Set(kRawRequestIds, std::move(raw_req_ids));
    ctx->Set(kRawDocs, std::move(raw_docs));
    ctx->Set(kRawQueries, std::move(raw_queries));
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

    auto* res = ctx->Get(kFinalDocOutputs);
    if (!res) {
      if (out_status) {
        *out_status = AdapterStatus::BufferTooSmall(
            "final_doc_outputs not found in AlgContext", "final_doc_outputs",
            -1, BizName());
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
      auto* out_ptr = static_cast<CompanyDocOutputStruct*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->confidence = (*res)[i].confidence;
      out_ptr->chunk_count = (*res)[i].chunk_count;
      out_ptr->status_code = (*res)[i].status_code;

      // RECHECK-001: 严格拦截截断
      if (!AdapterValidationHelper::CheckedCompanyStringWrite(
              out_ptr->intent_name, (*res)[i].intent_name.c_str(),
              "outputs[i].intent_name", i, BizName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }

      if (!AdapterValidationHelper::CheckedCompanyStringWrite(
              out_ptr->answer_text, (*res)[i].answer_text.c_str(),
              "outputs[i].answer_text", i, BizName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BUSINESS_ADAPTER(DocQaAdapter);

}  // namespace alg_framework

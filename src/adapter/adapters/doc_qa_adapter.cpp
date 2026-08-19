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
        {"smart_doc_qa_v1",
         "smart_doc_qa_onnx_llamacpp_v1"}};  // RECHECK-002: 精确白名单
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
      if (!AdapterValidationHelper::RequireBoundedString(
              "inputs[i].query_text", in_doc->query_text, kMaxQueryLen, i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      if (in_doc->doc_text) {
        if (!AdapterValidationHelper::RequireBoundedString(
                "inputs[i].doc_text", in_doc->doc_text, kMaxDocLen, i,
                BizName(), out_status)) {
          return COMPANY_ALG_ERR_INVALID_INPUT;
        }
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

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs,
           AdapterStatus* out_status = nullptr) const override {
    if (!ctx) {
      if (out_status) {
        *out_status = AdapterStatus::BufferTooSmall(
            "Null AlgContext passed to Pack", "ctx", -1, BizName());
      }
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }

    auto* res = ctx->Get<std::vector<DocQaResult>>("final_doc_outputs");
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
      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->intent_name, sizeof(out_ptr->intent_name),
              (*res)[i].intent_name.c_str(), "outputs[i].intent_name", i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }

      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->answer_text, sizeof(out_ptr->answer_text),
              (*res)[i].answer_text.c_str(), "outputs[i].answer_text", i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BUSINESS_ADAPTER(DocQaAdapter);

}  // namespace alg_framework

#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_adapter_registry.h"
#include "biz/ocr_doc_qa/ocr_doc_qa_contract.h"
#include "company_alg_interface.h"

namespace alg_framework {

class OcrDocQaAdapter : public IBizAdapter {
 public:
  CompanyAlgBizType BizType() const override { return ALG_BIZ_TYPE_OCR_DOC_QA; }

  const char* BizName() const override { return "OcrDocQA"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{
        ALG_BIZ_TYPE_OCR_DOC_QA,
        "OcrDocQA",
        "2.0.0",
        "CompanyOcrDocInputStruct",
        "CompanyOcrDocOutputStruct",
        64,
        OwnershipPolicy::kCopyIn,
        ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne,
        {{kOcrDocQaBusinessName,
          "ocr_doc_qa",
          "OCR 票据问答",
          {RequiredInput(kRawRequestIds), RequiredInput(kRawImagePaths),
           RequiredInput(kRawQueries)},
          {Output(kOcrDocFinalOutputs)}}}};
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
    std::vector<std::string> raw_images;
    std::vector<std::string> raw_queries;

    raw_req_ids.reserve(num_inputs);
    raw_images.reserve(num_inputs);
    raw_queries.reserve(num_inputs);

    constexpr size_t kMaxPathLen = 4096;
    constexpr size_t kMaxQueryLen = 64 * 1024;

    for (int i = 0; i < num_inputs; ++i) {
      auto* in_ocr = static_cast<const CompanyOcrDocInputStruct*>(inputs[i]);
      if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in_ocr, i,
                                                   BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      // ADP-001, RECHECK-004: 有界字符串强校验
      if (!AdapterValidationHelper::RequireBoundedString(
              "inputs[i].image_path", in_ocr->image_path, kMaxPathLen, i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }
      if (!AdapterValidationHelper::RequireBoundedString(
              "inputs[i].query_prompt", in_ocr->query_prompt, kMaxQueryLen, i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      raw_req_ids.push_back(in_ocr->request_id);
      raw_images.push_back(in_ocr->image_path);
      raw_queries.push_back(in_ocr->query_prompt);
    }

    ctx->Set(kRawRequestIds, std::move(raw_req_ids));
    ctx->Set(kRawImagePaths, std::move(raw_images));
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

    auto* res = ctx->Get(kOcrDocFinalOutputs);
    if (!res) {
      if (out_status) {
        *out_status = AdapterStatus::BufferTooSmall(
            "ocr_doc_final_outputs not found in AlgContext",
            "ocr_doc_final_outputs", -1, BizName());
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
      auto* out_ptr = static_cast<CompanyOcrDocOutputStruct*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->detected_box_count = (*res)[i].detected_box_count;
      out_ptr->status_code = (*res)[i].status_code;

      // RECHECK-001: 严格拦截截断
      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->extracted_invoice_json,
              sizeof(out_ptr->extracted_invoice_json),
              (*res)[i].extracted_invoice_json.c_str(),
              "outputs[i].extracted_invoice_json", i, BizName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BIZ_ADAPTER(OcrDocQaAdapter);

}  // namespace alg_framework

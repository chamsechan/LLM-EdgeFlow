#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/business_adapter_registry.h"
#include "business/ocr_doc_qa/ocr_doc_qa_dto.h"
#include "company_alg_interface.h"

namespace alg_framework {

class OcrDocQaAdapter : public IBusinessAdapter {
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
        OutputCardinality::kOneToOne};
    return desc;
  }

  bool ValidatePipelineBinding(
      const std::string& pipeline_biz_name) const override {
    return pipeline_biz_name.find("ocr") != std::string::npos ||
           pipeline_biz_name == "OcrDocQA";
  }

  int Unpack(const void** inputs, int num_inputs,
             AlgContext* ctx) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, BizName());
    if (valid_ret != 0 || !ctx) return COMPANY_ALG_ERR_INVALID_INPUT;

    std::vector<uint64_t> raw_req_ids;
    std::vector<std::string> raw_images;
    std::vector<std::string> raw_queries;

    raw_req_ids.reserve(num_inputs);
    raw_images.reserve(num_inputs);
    raw_queries.reserve(num_inputs);

    for (int i = 0; i < num_inputs; ++i) {
      auto* in_ocr = static_cast<const CompanyOcrDocInputStruct*>(inputs[i]);
      if (!in_ocr || !in_ocr->image_path || !in_ocr->query_prompt) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      raw_req_ids.push_back(in_ocr->request_id);
      raw_images.push_back(in_ocr->image_path);
      raw_queries.push_back(in_ocr->query_prompt);
    }

    ctx->Set("raw_request_ids", std::move(raw_req_ids));
    ctx->Set("raw_image_paths", std::move(raw_images));
    ctx->Set("raw_queries", std::move(raw_queries));
    return COMPANY_ALG_SUCCESS;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs) const override {
    if (!ctx) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    auto* res = ctx->Get<std::vector<OcrDocResult>>("ocr_doc_final_outputs");
    if (!res) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName());
    if (valid_ret != 0) return valid_ret;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<CompanyOcrDocOutputStruct*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->detected_box_count = (*res)[i].detected_box_count;
      out_ptr->status_code = (*res)[i].status_code;

      AdapterValidationHelper::CheckedStringCopy(
          out_ptr->extracted_invoice_json,
          sizeof(out_ptr->extracted_invoice_json),
          (*res)[i].extracted_invoice_json.c_str(),
          "outputs[i].extracted_invoice_json", i, BizName());
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BUSINESS_ADAPTER(OcrDocQaAdapter);

}  // namespace alg_framework

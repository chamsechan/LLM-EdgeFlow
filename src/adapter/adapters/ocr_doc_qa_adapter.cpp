#include <cstring>
#include <vector>

#include "adapter/business_adapter_registry.h"
#include "business/ocr_doc_qa/ocr_doc_qa_dto.h"
#include "company_alg_interface.h"

namespace alg_framework {

class OcrDocQaAdapter : public IBusinessAdapter {
 public:
  CompanyAlgBizType BizType() const override { return ALG_BIZ_TYPE_OCR_DOC_QA; }

  const char* BizName() const override { return "OcrDocQA"; }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx) override {
    if (!inputs || num_inputs <= 0 || !ctx) return -3;

    std::vector<uint64_t> raw_req_ids;
    std::vector<std::string> raw_images;
    std::vector<std::string> raw_queries;

    raw_req_ids.reserve(num_inputs);
    raw_images.reserve(num_inputs);
    raw_queries.reserve(num_inputs);

    for (int i = 0; i < num_inputs; ++i) {
      auto* in_ocr = static_cast<const CompanyOcrDocInputStruct*>(inputs[i]);
      if (!in_ocr) return -3;
      raw_req_ids.push_back(in_ocr->request_id);
      raw_images.push_back(in_ocr->image_path ? in_ocr->image_path : "");
      raw_queries.push_back(in_ocr->query_prompt ? in_ocr->query_prompt : "");
    }

    ctx->Set("raw_request_ids", std::move(raw_req_ids));
    ctx->Set("raw_image_paths", std::move(raw_images));
    ctx->Set("raw_queries", std::move(raw_queries));
    return 0;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs) override {
    if (!ctx || !outputs || !num_outputs || *num_outputs <= 0) return -4;

    auto* res = ctx->Get<std::vector<OcrDocResult>>("ocr_doc_final_outputs");
    if (!res) return -4;

    int count = static_cast<int>(res->size());
    int out_limit = *num_outputs;
    for (int i = 0; i < count && i < out_limit; ++i) {
      auto* out_ptr = static_cast<CompanyOcrDocOutputStruct*>(outputs[i]);
      if (out_ptr) {
        out_ptr->request_id = (*res)[i].request_id;
        out_ptr->detected_box_count = (*res)[i].detected_box_count;
        out_ptr->status_code = (*res)[i].status_code;

        strncpy(out_ptr->extracted_invoice_json,
                (*res)[i].extracted_invoice_json.c_str(),
                sizeof(out_ptr->extracted_invoice_json) - 1);
        out_ptr
            ->extracted_invoice_json[sizeof(out_ptr->extracted_invoice_json) -
                                     1] = '\0';
      }
    }
    *num_outputs = count;
    return 0;
  }
};

REGISTER_BUSINESS_ADAPTER(OcrDocQaAdapter);

}  // namespace alg_framework

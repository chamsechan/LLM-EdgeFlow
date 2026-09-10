#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_adapter_registry.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/biz_results.h"
#include "adapter/result_packing_adapter.h"
#include "adapter/result_validation.h"
#include "edgeflow/c_api.h"

namespace llm_edgeflow {

inline static constexpr char kOcrDocQaBizName[] = "multimodal_ocr_invoice_qa";

class OcrDocQaAdapter
    : public ResultPackingAdapter<OcrDocQaAdapter, CompanyOcrDocOutputStruct,
                                  OcrDocResult> {
 public:
  CompanyAlgBizType BizType() const override { return ALG_BIZ_TYPE_OCR_DOC_QA; }

  const char* AdapterName() const override { return "OcrDocQA"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{
        ALG_BIZ_TYPE_OCR_DOC_QA,
        "OcrDocQA",
        COMPANY_ALG_ABI_VERSION,
        "CompanyOcrDocInputStruct",
        "CompanyOcrDocOutputStruct",
        64,
        OwnershipPolicy::kCopyIn,
        ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne,
        {{kOcrDocQaBizName,
          "ocr_doc_qa",
          "OCR 票据问答",
          {RequiredBizInput(kRawRequestIds), RequiredBizInput(kImagePaths),
           RequiredBizInput(kUserQueries)},
          {BizOutput(kExtractedInvoiceJson), BizOutput(kOcrDocs)}}}};
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx,
             AdapterStatus* out_status = nullptr) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, AdapterName());
    if (valid_ret != 0 || !ctx) {
      return AdapterValidationHelper::ReturnInvalidInput(
          out_status, "Batch envelope validation failed or null AlgContext",
          "inputs", AdapterName());
    }

    std::vector<uint64_t> raw_req_ids;
    ImageRefBatch raw_images;
    TextBatch raw_queries;

    raw_req_ids.reserve(num_inputs);
    raw_images.reserve(num_inputs);
    raw_queries.reserve(num_inputs);

    constexpr size_t kMaxPathLen = 4096;
    constexpr size_t kMaxQueryLen = 64 * 1024;

    for (int i = 0; i < num_inputs; ++i) {
      auto* in_ocr = static_cast<const CompanyOcrDocInputStruct*>(inputs[i]);
      if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in_ocr, i,
                                                   AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      if (!AdapterValidationHelper::RequireBoundedString(
              "inputs[i].image_path", in_ocr->image_path, kMaxPathLen, i,
              AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }
      if (!AdapterValidationHelper::RequireBoundedString(
              "inputs[i].query_prompt", in_ocr->query_prompt, kMaxQueryLen, i,
              AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      raw_req_ids.push_back(in_ocr->request_id);
      raw_images.emplace_back(static_cast<uint32_t>(i), 0, in_ocr->image_path);
      raw_queries.emplace_back(
          static_cast<uint32_t>(i), 0,
          in_ocr->query_prompt ? in_ocr->query_prompt : "");
    }

    if (!AdapterValidationHelper::PublishContextValue(
            *ctx, kRawRequestIds, std::move(raw_req_ids), AdapterName(),
            out_status) ||
        !AdapterValidationHelper::PublishContextValue(
            *ctx, kImagePaths, std::move(raw_images), AdapterName(),
            out_status) ||
        !AdapterValidationHelper::PublishContextValue(
            *ctx, kUserQueries, std::move(raw_queries), AdapterName(),
            out_status)) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    return COMPANY_ALG_SUCCESS;
  }

  template <typename Output>
  int PackTyped(AlgContext* ctx, void** outputs, int* num_outputs,
                AdapterStatus* out_status = nullptr) const {
    if (!ctx) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          out_status, "Null AlgContext passed to Pack", "ctx", AdapterName());
    }

    const auto* invoice_jsons =
        AdapterValidationHelper::ReadRequiredContextValue(
            *ctx, kExtractedInvoiceJson, AdapterName(), out_status);
    if (!invoice_jsons) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    const auto* ocr_docs = ctx->Read(kOcrDocs);
    const auto* raw_req_ids = ctx->Read(kRawRequestIds);

    int count = static_cast<int>(invoice_jsons->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, AdapterName(), out_status);
    if (valid_ret != 0) return valid_ret;

    std::vector<const StructuredDocumentBatch::value_type*>
        invoice_jsons_by_request;
    if (!IndexResults(invoice_jsons, raw_req_ids, &invoice_jsons_by_request,
                      "invoice_jsons", AdapterName(), out_status))
      return COMPANY_ALG_ERR_INVALID_INPUT;
    std::vector<const OcrDocumentBatch::value_type*> ocr_docs_by_request;
    if (!IndexResults(ocr_docs, raw_req_ids, &ocr_docs_by_request, "ocr_docs",
                      AdapterName(), out_status))
      return COMPANY_ALG_ERR_INVALID_INPUT;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<Output*>(outputs[i]);
      uint64_t req_id =
          (raw_req_ids && i < static_cast<int>(raw_req_ids->size()))
              ? (*raw_req_ids)[i]
              : invoice_jsons_by_request[i]->req_id;
      out_ptr->request_id = req_id;

      int box_count = 0;
      if (ocr_docs && i < static_cast<int>(ocr_docs->size())) {
        box_count = static_cast<int>(ocr_docs_by_request[i]->data.boxes.size());
      }
      out_ptr->detected_box_count = box_count;
      if (!IsSuccessfulDocument(invoice_jsons_by_request[i]->data)) {
        return AdapterValidationHelper::ReturnInvalidInput(
            out_status, "Structured result failed or used fallback",
            "invoice_jsons", AdapterName(), i);
      }
      out_ptr->status_code = 0;

      if (!CopyResultString(
              out_ptr->extracted_invoice_json,
              invoice_jsons_by_request[i]->data.json_payload.c_str(),
              "outputs[i].extracted_invoice_json", i, AdapterName(),
              out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BIZ_ADAPTER(OcrDocQaAdapter);

}  // namespace llm_edgeflow

#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_adapter_registry.h"
#include "company_alg_interface.h"
#include "core/common_contracts.h"

namespace alg_framework {

inline static constexpr char kOcrDocQaBizName[] = "multimodal_ocr_invoice_qa";

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
        {{kOcrDocQaBizName,
          "ocr_doc_qa",
          "OCR 票据问答",
          {RequiredInput(kRawRequestIds), RequiredInput(kImagePaths),
           RequiredInput(kUserQueries)},
          {Output(kExtractedInvoiceJson), Output(kOcrDocs)}}}};
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx,
             AdapterStatus* out_status = nullptr) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, BizName());
    if (valid_ret != 0 || !ctx) {
      return AdapterValidationHelper::ReturnInvalidInput(
          out_status, "Batch envelope validation failed or null AlgContext",
          "inputs", BizName());
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
                                                   BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

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
      raw_images.emplace_back(static_cast<uint32_t>(i), 0, in_ocr->image_path);
      raw_queries.emplace_back(
          static_cast<uint32_t>(i), 0,
          in_ocr->query_prompt ? in_ocr->query_prompt : "");
    }

    if (!AdapterValidationHelper::PublishContextValue(*ctx, kRawRequestIds,
                                                      std::move(raw_req_ids),
                                                      BizName(), out_status) ||
        !AdapterValidationHelper::PublishContextValue(
            *ctx, kImagePaths, std::move(raw_images), BizName(), out_status) ||
        !AdapterValidationHelper::PublishContextValue(*ctx, kUserQueries,
                                                      std::move(raw_queries),
                                                      BizName(), out_status)) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    return COMPANY_ALG_SUCCESS;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs,
           AdapterStatus* out_status = nullptr) const override {
    if (!ctx) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          out_status, "Null AlgContext passed to Pack", "ctx", BizName());
    }

    const auto* invoice_jsons =
        AdapterValidationHelper::ReadRequiredContextValue(
            *ctx, kExtractedInvoiceJson, BizName(), out_status);
    if (!invoice_jsons) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    const auto* ocr_docs = ctx->Read(kOcrDocs);
    const auto* raw_req_ids = ctx->Read(kRawRequestIds);

    int count = static_cast<int>(invoice_jsons->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName(), out_status);
    if (valid_ret != 0) return valid_ret;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<CompanyOcrDocOutputStruct*>(outputs[i]);
      uint64_t req_id =
          (raw_req_ids && i < static_cast<int>(raw_req_ids->size()))
              ? (*raw_req_ids)[i]
              : (*invoice_jsons)[i].req_id;
      out_ptr->request_id = req_id;

      int box_count = 0;
      if (ocr_docs && i < static_cast<int>(ocr_docs->size())) {
        box_count = static_cast<int>((*ocr_docs)[i].data.boxes.size());
      }
      out_ptr->detected_box_count = box_count;
      out_ptr->status_code = 0;

      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->extracted_invoice_json,
              sizeof(out_ptr->extracted_invoice_json),
              (*invoice_jsons)[i].data.json_payload.c_str(),
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

#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/result_validation.h"
#include "contracts/inference_payloads.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kOutputSlot = "od_out";

int EncodeOperatorInvoiceResult(AlgContext* context,
                                const OutputEncodeOptions& options,
                                ExternalOutputBatchView* destination,
                                size_t* written_count, AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Encode", "context",
        options.Label().c_str());
  }

  const auto* invoice_jsons =
      ReadOutputValue(*context, kExtractedInvoiceJson, options, status);
  if (!invoice_jsons) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* ocr_docs = ReadOutputValue(*context, kOcrDocs, options, status);
  if (!ocr_docs) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* raw_req_ids = RequestIds(options, status);
  if (!raw_req_ids) return COMPANY_ALG_ERR_INVALID_INPUT;

  size_t count = invoice_jsons->size();
  if (destination->count < count) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Destination item count is less than output count",
        "destination", options.Label().c_str());
  }

  std::vector<const StructuredDocumentBatch::value_type*>
      invoice_jsons_by_request;
  if (!IndexResults(invoice_jsons, raw_req_ids, &invoice_jsons_by_request,
                    "invoice_jsons", options.Label().c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }
  std::vector<const OcrDocumentBatch::value_type*> ocr_docs_by_request;
  if (!IndexResults(ocr_docs, raw_req_ids, &ocr_docs_by_request, "ocr_docs",
                    options.Label().c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  for (size_t i = 0; i < count; ++i) {
    auto* out = destination->GetSlot<CompanyOdOutput>(kOutputSlot, i);
    if (!out) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status, "Missing od_out slot item", kOutputSlot,
          options.Label().c_str(), static_cast<int>(i));
    }

    out->request_id = (*raw_req_ids)[i];
    out->detected_box_count =
        static_cast<int>(ocr_docs_by_request[i]->data.boxes.size());
    if (!IsSuccessfulDocument(invoice_jsons_by_request[i]->data)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Structured result failed or used fallback", "invoice_jsons",
          options.Label().c_str(), static_cast<int>(i));
    }
    out->status_code = 0;

    if (!WriteOutputString(*destination, kOutputSlot, out->result_json,
                           "result_json",
                           invoice_jsons_by_request[i]->data.json_payload,
                           options, status, i)) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
  }

  if (written_count) *written_count = count;
  return COMPANY_ALG_SUCCESS;
}

// 输出字符串字段的尺寸参数；默认值按该业务的载荷设定，上限由平台结构登记决定。
struct Params {
  int64_t result_json_max_bytes{};
};

auto ParamSpec() {
  return Parameters<Params>(
      {MaxBytes("result_json", &Params::result_json_max_bytes).Default(2047)});
}

OutputConverterDefinition MakeOperatorInvoiceResultOutputConverter() {
  OutputConverterDefinition def;
  def.type = kOutputSlot;
  def.name = "ocr_invoice_qa";
  def.service_type =
      COMPANY_MOCK_SERVICE_OCR_INVOICE_QA;  // 占位取值，进内网核对
  def.slot = ExternalOutputSlot<CompanyOdOutput>(kOutputSlot);
  def.logical_ports = {RequiredInputPort(kExtractedInvoiceJson),
                       RequiredInputPort(kOcrDocs)};
  def.params = ParamSpec();
  def.encode_fn = &EncodeOperatorInvoiceResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorInvoiceResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow

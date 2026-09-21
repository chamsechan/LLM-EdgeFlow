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

int EncodeOperatorInvoiceResult(AlgContext* context,
                                const OutputPortBindings& bindings,
                                const OutputEncodeOptions& options,
                                ExternalOutputBatchView* destination,
                                size_t* written_count, AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Encode", "context",
        options.converter_id.c_str());
  }

  const auto* invoice_jsons = ReadOutputValue(
      *context, bindings, kExtractedInvoiceJson, options, status);
  if (!invoice_jsons) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* ocr_docs =
      ReadOutputValue(*context, bindings, kOcrDocs, options, status);
  if (!ocr_docs) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* raw_req_ids =
      ReadOutputValue(*context, bindings, kRawRequestIds, options, status);
  if (!raw_req_ids) return COMPANY_ALG_ERR_INVALID_INPUT;

  size_t count = invoice_jsons->size();
  if (destination->count < count) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Destination item count is less than output count",
        "destination", options.converter_id.c_str());
  }

  std::vector<const StructuredDocumentBatch::value_type*>
      invoice_jsons_by_request;
  if (!IndexResults(invoice_jsons, raw_req_ids, &invoice_jsons_by_request,
                    "invoice_jsons", options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }
  std::vector<const OcrDocumentBatch::value_type*> ocr_docs_by_request;
  if (!IndexResults(ocr_docs, raw_req_ids, &ocr_docs_by_request, "ocr_docs",
                    options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  for (size_t i = 0; i < count; ++i) {
    auto* out = destination->GetSlot<CompanyOdOutput>("od_out", i);
    if (!out) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status, "Missing od_out slot item", "od_out",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    out->request_id = (*raw_req_ids)[i];
    out->detected_box_count =
        static_cast<int>(ocr_docs_by_request[i]->data.boxes.size());
    if (!IsSuccessfulDocument(invoice_jsons_by_request[i]->data)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Structured result failed or used fallback", "invoice_jsons",
          options.converter_id.c_str(), static_cast<int>(i));
    }
    out->status_code = 0;

    if (!WriteOutputString(
            *destination, "od_out", out->result_json, "result_json",
            invoice_jsons_by_request[i]->data.json_payload.c_str(), options,
            status, i)) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
  }

  if (written_count) *written_count = count;
  return COMPANY_ALG_SUCCESS;
}

OutputConverterDefinition MakeOperatorInvoiceResultOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "invoice_result.plain.operator.v1";

  def.schema_id = "invoice_result.plain.response";
  def.external_type = "CompanyOdOutput";
  def.max_batch_size = 64;

  def.external_slots = {
      ExternalOutputSlot<CompanyOdOutput>("od_out", {"result_json"})};
  def.logical_ports = {RequiredInputPort(kRawRequestIds),
                       RequiredInputPort(kExtractedInvoiceJson),
                       RequiredInputPort(kOcrDocs)};
  def.encode_fn = &EncodeOperatorInvoiceResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorInvoiceResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow

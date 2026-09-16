#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/result_validation.h"
#include "contracts/inference_payloads.h"
#include "edgeflow/c_api.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

int EncodeCAbiInvoiceResult(AlgContext* context,
                            const OutputPortBindings& bindings,
                            const OutputEncodeOptions& options,
                            ExternalOutputBatchView* destination,
                            size_t* written_count, AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Null AlgContext passed to Encode", "context",
        options.converter_id.c_str());
  }

  const auto* invoice_jsons = context->Read<StructuredDocumentBatch>(
      bindings.GetActualKey("extracted_invoice_json"));
  if (!invoice_jsons) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: extracted_invoice_json",
        "extracted_invoice_json", options.converter_id.c_str());
  }

  const auto* ocr_docs =
      context->Read<OcrDocumentBatch>(bindings.GetActualKey("ocr_docs"));
  if (!ocr_docs) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Missing required context value: ocr_docs", "ocr_docs",
        options.converter_id.c_str());
  }

  const auto* raw_req_ids = context->Read<std::vector<uint64_t>>(
      bindings.GetActualKey("raw_request_ids"));
  if (!raw_req_ids) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: raw_request_ids",
        "raw_request_ids", options.converter_id.c_str());
  }

  int count = static_cast<int>(invoice_jsons->size());
  int cap = static_cast<int>(destination->capacity > 0 ? destination->capacity
                                                       : destination->count);
  int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
      destination->items, &cap, count, options.converter_id.c_str(), status);
  if (valid_ret != 0) return valid_ret;

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

  for (int i = 0; i < count; ++i) {
    auto* out_ptr = destination->GetCAbi<CompanyOcrDocOutputStruct>(i);
    out_ptr->request_id = (*raw_req_ids)[i];
    out_ptr->detected_box_count =
        static_cast<int>(ocr_docs_by_request[i]->data.boxes.size());
    if (!IsSuccessfulDocument(invoice_jsons_by_request[i]->data)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Structured result failed or used fallback", "invoice_jsons",
          options.converter_id.c_str(), i);
    }
    out_ptr->status_code = 0;

    if (!AdapterValidationHelper::CheckedStringCopy(
            out_ptr->extracted_invoice_json,
            sizeof(out_ptr->extracted_invoice_json),
            invoice_jsons_by_request[i]->data.json_payload.c_str(),
            "outputs[i].extracted_invoice_json", i,
            options.converter_id.c_str(), status)) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
  }

  if (written_count) *written_count = static_cast<size_t>(count);
  return COMPANY_ALG_SUCCESS;
}

int EncodeOperatorInvoiceResult(AlgContext* context,
                                const OutputPortBindings& bindings,
                                const OutputEncodeOptions& options,
                                ExternalOutputBatchView* destination,
                                size_t* written_count, AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Null AlgContext passed to Encode", "context",
        options.converter_id.c_str());
  }

  const auto* invoice_jsons = context->Read<StructuredDocumentBatch>(
      bindings.GetActualKey("extracted_invoice_json"));
  if (!invoice_jsons) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: extracted_invoice_json",
        "extracted_invoice_json", options.converter_id.c_str());
  }

  const auto* ocr_docs =
      context->Read<OcrDocumentBatch>(bindings.GetActualKey("ocr_docs"));
  if (!ocr_docs) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Missing required context value: ocr_docs", "ocr_docs",
        options.converter_id.c_str());
  }

  const auto* raw_req_ids = context->Read<std::vector<uint64_t>>(
      bindings.GetActualKey("raw_request_ids"));
  if (!raw_req_ids) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: raw_request_ids",
        "raw_request_ids", options.converter_id.c_str());
  }

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

    std::string err;
    int ret = CopyToOperatorString(
        invoice_jsons_by_request[i]->data.json_payload.c_str(),
        out->result_json,
        destination->GetSlotCapacity("od_out", "result_json", 1023),
        "result_json", &err);
    if (ret != 0) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status,
          err.empty() ? "Buffer too small for result_json" : err.c_str(),
          "result_json", options.converter_id.c_str(), static_cast<int>(i));
    }
  }

  if (written_count) *written_count = count;
  return COMPANY_ALG_SUCCESS;
}

OutputConverterDefinition MakeCAbiInvoiceResultOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "invoice_result.plain.cabi.v1";
  def.transport = "cabi";
  def.schema_id = "invoice_result.plain.response";
  def.schema_version = 1;
  def.external_type = "CompanyOcrDocOutputStruct";
  def.cardinality = "1:1";
  def.max_batch_size = 64;
  def.capacity_policy = "reject_overflow";
  def.thread_model = "stateless";
  def.external_slots = {{"outputs",
                         "CompanyOcrDocOutputStruct",
                         PortDirection::kOutput,
                         true,
                         "CompanyOcrDocOutputStruct",
                         "",
                         {"extracted_invoice_json"}}};
  def.logical_ports = {
      NodePortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      NodePortDefinition("extracted_invoice_json", "StructuredDocumentBatch",
                         true, "1:1"),
      NodePortDefinition("ocr_docs", "OcrDocumentBatch", true, "1:1")};
  def.encode_fn = &EncodeCAbiInvoiceResult;
  return def;
}

OutputConverterDefinition MakeOperatorInvoiceResultOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "invoice_result.plain.operator.v1";
  def.transport = "operator";
  def.schema_id = "invoice_result.plain.response";
  def.schema_version = 1;
  def.external_type = "CompanyOdOutput";
  def.cardinality = "1:1";
  def.max_batch_size = 64;
  def.capacity_policy = "reject_overflow";
  def.thread_model = "stateless";
  def.external_slots = {{"od_out",
                         "CompanyOdOutput",
                         PortDirection::kOutput,
                         true,
                         "CompanyOdOutput",
                         "od_out",
                         {"result_json"}}};
  def.logical_ports = {
      NodePortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      NodePortDefinition("extracted_invoice_json", "StructuredDocumentBatch",
                         true, "1:1"),
      NodePortDefinition("ocr_docs", "OcrDocumentBatch", true, "1:1")};
  def.encode_fn = &EncodeOperatorInvoiceResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeCAbiInvoiceResultOutputConverter());
REGISTER_OUTPUT_CONVERTER(MakeOperatorInvoiceResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow

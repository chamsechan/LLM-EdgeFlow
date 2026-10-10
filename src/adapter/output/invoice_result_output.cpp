#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_values.h"
#include "adapter/result_validation.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"

namespace llm_edgeflow {
namespace {

constexpr auto kResult = MakeBlackboardKey<StructuredDocumentBatch>("result");
constexpr auto kDocument = MakeBlackboardKey<OcrDocumentBatch>("document");

constexpr const char* kOutputSlot = "od_out";

struct Params {
  int64_t result_json_max_bytes = 0;
};

Parameters<Params> ParamSpec() {
  return Parameters<Params>({
      MaxBytes("result_json", &Params::result_json_max_bytes).Default(2047),
  });
}

int EncodeOperatorInvoiceResult(AlgContext* context,
                                const OutputEncodeOptions& options,
                                ExternalOutputBatchView* destination,
                                size_t* written_count, AdapterStatus* status) {
  if (written_count) *written_count = 0;
  if (!context) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Encode", "context",
        options.Label().c_str());
  }

  const auto* invoice_jsons =
      ReadOutputValue(*context, kResult, options, status);
  if (!invoice_jsons) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* ocr_docs = ReadOutputValue(*context, kDocument, options, status);
  if (!ocr_docs) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* raw_req_ids = RequestIds(options, status);
  if (!raw_req_ids) return COMPANY_ALG_ERR_INVALID_INPUT;

  size_t count = invoice_jsons->size();
  if (!destination || destination->count < count) {
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

  size_t written = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!destination->HasSlot(kOutputSlot, i) && !destination->required) {
      continue;
    }
    DetectionOutputValue out;

    out.detected_box_count =
        static_cast<int>(ocr_docs_by_request[i]->data.boxes.size());
    if (!IsSuccessfulDocument(invoice_jsons_by_request[i]->data)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Structured result failed or used fallback", "invoice_jsons",
          options.Label().c_str(), static_cast<int>(i));
    }
    out.status_code = 0;

    out.result_json = invoice_jsons_by_request[i]->data.json_payload;
    if (!WriteOutputValue(*destination, kOutputSlot, i, out, options, status)) {
      return status ? status->Code() : COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
    ++written;
  }

  if (written_count) *written_count = written;
  return COMPANY_ALG_SUCCESS;
}

OutputConverterDefinition MakeOperatorInvoiceResultOutputConverter() {
  OutputConverterDefinition def;
  def.type = kOutputSlot;
  def.name = "ocr_invoice_qa";
  def.slot = ExternalOutputSlot<DetectionOutputValue>(kOutputSlot);
  def.logical_ports = {RequiredInputPort(kResult),
                       RequiredInputPort(kDocument)};
  def.params = ParamSpec();
  def.encode_fn = &EncodeOperatorInvoiceResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorInvoiceResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow

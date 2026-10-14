#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_values.h"
#include "adapter/json_output_parameters.h"
#include "adapter/result_validation.h"
#include "contracts/error_codes.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"
#include "nlohmann/json.hpp"

namespace llm_edgeflow {
namespace {

constexpr auto kResult = MakeBlackboardKey<StructuredDocumentBatch>("result");
constexpr auto kDocument = MakeBlackboardKey<OcrDocumentBatch>("document");

constexpr const char* kOutputSlot = "json_out";

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
  if (!invoice_jsons) return error::kInvalidInput;

  const auto* ocr_docs = ReadOutputValue(*context, kDocument, options, status);
  if (!ocr_docs) return error::kInvalidInput;

  size_t count = invoice_jsons->size();
  if (!destination || destination->count < count) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Destination item count is less than output count",
        "destination", options.Label().c_str());
  }

  std::vector<const StructuredDocumentBatch::value_type*>
      invoice_jsons_by_request;
  if (!IndexResults(invoice_jsons, destination->count,
                    &invoice_jsons_by_request, "invoice_jsons",
                    options.Label().c_str(), status)) {
    return error::kInvalidInput;
  }
  std::vector<const OcrDocumentBatch::value_type*> ocr_docs_by_request;
  if (!IndexResults(ocr_docs, destination->count, &ocr_docs_by_request,
                    "ocr_docs", options.Label().c_str(), status)) {
    return error::kInvalidInput;
  }

  size_t written = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!destination->HasSlot(kOutputSlot, i) && !destination->required) {
      continue;
    }
    JsonOutputValue out;

    if (!IsSuccessfulDocument(invoice_jsons_by_request[i]->data)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Structured result failed or used fallback", "invoice_jsons",
          options.Label().c_str(), static_cast<int>(i));
    }
    out.status_code = 0;

    out.result_json =
        nlohmann::json(
            {{"detected_box_count", ocr_docs_by_request[i]->data.boxes.size()},
             {"result", nlohmann::json::parse(
                            invoice_jsons_by_request[i]->data.json_payload)}})
            .dump();
    if (!WriteOutputValue(*destination, kOutputSlot, i, out, options, status)) {
      return status ? status->Code() : error::kInvalidOutput;
    }
    ++written;
  }

  if (written_count) *written_count = written;
  return error::kOk;
}

OutputConverterDefinition MakeOperatorInvoiceResultOutputConverter() {
  OutputConverterDefinition def;
  def.type = kOutputSlot;
  def.name = "ocr_invoice_qa";
  def.slot = ExternalOutputSlot<JsonOutputValue>();
  def.logical_ports = {RequiredInputPort(kResult),
                       RequiredInputPort(kDocument)};
  def.params = JsonOutputParameters();
  def.encode_fn = &EncodeOperatorInvoiceResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorInvoiceResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow

#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/biz_results.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/result_validation.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

int EncodeOperatorStructuredDocument(AlgContext* context,
                                     const OutputPortBindings& bindings,
                                     const OutputEncodeOptions& options,
                                     ExternalOutputBatchView* destination,
                                     size_t* written_count,
                                     AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Encode", "context",
        options.converter_id.c_str());
  }

  const auto* res = ReadOutputValue(*context, bindings, kExtractedEntities,
                                    options, status, "res");
  if (!res) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* raw_req_ids =
      ReadOutputValue(*context, bindings, kRawRequestIds, options, status);
  if (!raw_req_ids) return COMPANY_ALG_ERR_INVALID_INPUT;

  size_t count = res->size();
  if (!destination || destination->count < count) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Destination item count is less than output count",
        "destination", options.converter_id.c_str());
  }

  std::vector<const StructuredDocumentBatch::value_type*> res_by_request;
  if (!IndexResults(res, raw_req_ids, &res_by_request, "res",
                    options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  for (size_t i = 0; i < count; ++i) {
    auto* out =
        destination->GetSlot<CompanyOperatorEntityOutput>("entity_out", i);
    if (!out) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status, "Missing entity_out slot block in output view", "entity_out",
          options.converter_id.c_str(), static_cast<int>(i));
    }
    out->request_id = (*raw_req_ids)[i];
    if (!IsSuccessfulDocument(res_by_request[i]->data)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Structured result failed or used fallback", "res",
          options.converter_id.c_str(), static_cast<int>(i));
    }
    out->status_code = 0;

    if (!WriteOutputString(
            *destination, "entity_out", out->entities_json, "entities_json",
            res_by_request[i]->data.json_payload.c_str(), options, status, i)) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
  }

  if (written_count) *written_count = count;
  return COMPANY_ALG_SUCCESS;
}

OutputConverterDefinition MakeOperatorStructuredDocumentOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "document.structured.operator.v1";

  def.schema_id = "document.structured.response";
  def.external_type = "CompanyOperatorEntityOutput";
  def.max_batch_size = 64;

  def.external_slots = {ExternalOutputSlot<CompanyOperatorEntityOutput>(
      "entity_out", {"entities_json"})};
  def.logical_ports = {RequiredInputPort(kRawRequestIds),
                       RequiredInputPort(kExtractedEntities)};
  def.encode_fn = &EncodeOperatorStructuredDocument;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorStructuredDocumentOutputConverter());

}  // namespace
}  // namespace llm_edgeflow

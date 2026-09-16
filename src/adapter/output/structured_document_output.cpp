#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_results.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/result_validation.h"
#include "edgeflow/c_api.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

int EncodeCAbiStructuredDocument(AlgContext* context,
                                 const OutputPortBindings& bindings,
                                 const OutputEncodeOptions& options,
                                 ExternalOutputBatchView* destination,
                                 size_t* written_count, AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Null AlgContext passed to Encode", "context",
        options.converter_id.c_str());
  }

  const auto* res = context->Read<StructuredDocumentBatch>(
      bindings.GetActualKey("extracted_entities"));
  if (!res) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: extracted_entities", "res",
        options.converter_id.c_str());
  }

  const auto* raw_req_ids = context->Read<std::vector<uint64_t>>(
      bindings.GetActualKey("raw_request_ids"));
  if (!raw_req_ids) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: raw_request_ids",
        "raw_request_ids", options.converter_id.c_str());
  }

  int count = static_cast<int>(res->size());
  int cap = static_cast<int>(destination->capacity > 0 ? destination->capacity
                                                       : destination->count);
  int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
      destination->items, &cap, count, options.converter_id.c_str(), status);
  if (valid_ret != 0) return valid_ret;

  std::vector<const StructuredDocumentBatch::value_type*> res_by_request;
  if (!IndexResults(res, raw_req_ids, &res_by_request, "res",
                    options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  for (int i = 0; i < count; ++i) {
    auto* out_ptr = destination->GetCAbi<CompanyEntityOutputStruct>(i);
    out_ptr->request_id = (*raw_req_ids)[i];
    if (!IsSuccessfulDocument(res_by_request[i]->data)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Structured result failed or used fallback", "res",
          options.converter_id.c_str(), i);
    }
    out_ptr->status_code = 0;

    if (!AdapterValidationHelper::CheckedStringCopy(
            out_ptr->entities_json, sizeof(out_ptr->entities_json),
            res_by_request[i]->data.json_payload.c_str(),
            "outputs[i].entities_json", i, options.converter_id.c_str(),
            status)) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
  }

  if (written_count) *written_count = static_cast<size_t>(count);
  return COMPANY_ALG_SUCCESS;
}

int EncodeOperatorStructuredDocument(AlgContext* context,
                                     const OutputPortBindings& bindings,
                                     const OutputEncodeOptions& options,
                                     ExternalOutputBatchView* destination,
                                     size_t* written_count,
                                     AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Null AlgContext passed to Encode", "context",
        options.converter_id.c_str());
  }

  const auto* res = context->Read<StructuredDocumentBatch>(
      bindings.GetActualKey("extracted_entities"));
  if (!res) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: extracted_entities", "res",
        options.converter_id.c_str());
  }

  const auto* raw_req_ids = context->Read<std::vector<uint64_t>>(
      bindings.GetActualKey("raw_request_ids"));
  if (!raw_req_ids) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: raw_request_ids",
        "raw_request_ids", options.converter_id.c_str());
  }

  size_t count = res->size();
  std::vector<const StructuredDocumentBatch::value_type*> res_by_request;
  if (!IndexResults(res, raw_req_ids, &res_by_request, "res",
                    options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  std::string diag_err;
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

    uint32_t cap =
        destination->GetSlotCapacity("entity_out", "entities_json", 2047);
    int ret = CopyToOperatorString(res_by_request[i]->data.json_payload.c_str(),
                                   out->entities_json, cap, "entities_json",
                                   &diag_err);
    if (ret != 0) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status, diag_err.c_str(), "entities_json",
          options.converter_id.c_str(), static_cast<int>(i));
    }
  }

  if (written_count) *written_count = count;
  return COMPANY_ALG_SUCCESS;
}

OutputConverterDefinition MakeCAbiStructuredDocumentOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "document.structured.cabi.v1";
  def.transport = "cabi";
  def.schema_id = "document.structured.response";
  def.schema_version = 1;
  def.external_type = "CompanyEntityOutputStruct";
  def.cardinality = "1:1";
  def.max_batch_size = 64;
  def.capacity_policy = "reject_overflow";
  def.thread_model = "stateless";
  def.external_slots = {{"entities_json",
                         "CompanyEntityOutputStruct",
                         PortDirection::kOutput,
                         true,
                         "CompanyEntityOutputStruct",
                         "",
                         {"entities_json"}}};
  def.logical_ports = {
      NodePortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      NodePortDefinition("extracted_entities", "StructuredDocumentBatch", true,
                         "1:1")};
  def.encode_fn = &EncodeCAbiStructuredDocument;
  return def;
}

OutputConverterDefinition MakeOperatorStructuredDocumentOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "document.structured.operator.v1";
  def.transport = "operator";
  def.schema_id = "document.structured.response";
  def.schema_version = 1;
  def.external_type = "CompanyOperatorEntityOutput";
  def.cardinality = "1:1";
  def.max_batch_size = 64;
  def.capacity_policy = "reject_overflow";
  def.thread_model = "stateless";
  def.external_slots = {{"entity_out",
                         "CompanyOperatorEntityOutput",
                         PortDirection::kOutput,
                         true,
                         "CompanyOperatorEntityOutput",
                         "entity_out",
                         {"entities_json"}}};
  def.logical_ports = {
      NodePortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      NodePortDefinition("extracted_entities", "StructuredDocumentBatch", true,
                         "1:1")};
  def.encode_fn = &EncodeOperatorStructuredDocument;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeCAbiStructuredDocumentOutputConverter());
REGISTER_OUTPUT_CONVERTER(MakeOperatorStructuredDocumentOutputConverter());

}  // namespace
}  // namespace llm_edgeflow

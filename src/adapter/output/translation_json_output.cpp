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
#include "nlohmann/json.hpp"

namespace llm_edgeflow {
namespace {

int EncodeCAbiTranslationJson(AlgContext* context,
                              const OutputPortBindings& bindings,
                              const OutputEncodeOptions& options,
                              ExternalOutputBatchView* destination,
                              size_t* written_count, AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Null AlgContext passed to Encode", "context",
        options.converter_id.c_str());
  }

  const auto* res =
      context->Read<TextBatch>(bindings.GetActualKey("llm_answers"));
  if (!res) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: llm_answers", "res",
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
  std::vector<const TextBatch::value_type*> res_by_request;
  if (!IndexResults(res, raw_req_ids, &res_by_request, "res",
                    options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  std::vector<std::string> payloads(count);
  for (int i = 0; i < count; ++i) {
    nlohmann::json response = {{"translated", res_by_request[i]->data}};
    payloads[i] = response.dump();
  }

  int cap = static_cast<int>(destination->capacity > 0 ? destination->capacity
                                                       : destination->count);
  int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
      destination->items, &cap, count, options.converter_id.c_str(), status);
  if (valid_ret != 0) return valid_ret;

  for (int i = 0; i < count; ++i) {
    auto* out_ptr = destination->GetCAbi<CompanyEntityOutputStruct>(i);
    out_ptr->request_id = (*raw_req_ids)[i];
    out_ptr->status_code = 0;

    if (!AdapterValidationHelper::CheckedStringCopy(
            out_ptr->entities_json, sizeof(out_ptr->entities_json),
            payloads[i].c_str(), "outputs[i].entities_json", i,
            options.converter_id.c_str(), status)) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
  }

  if (written_count) *written_count = static_cast<size_t>(count);
  return COMPANY_ALG_SUCCESS;
}

int EncodeOperatorTranslationJson(AlgContext* context,
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

  const auto* res =
      context->Read<TextBatch>(bindings.GetActualKey("llm_answers"));
  if (!res) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: llm_answers", "res",
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
  std::vector<const TextBatch::value_type*> res_by_request;
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
    out->status_code = 0;

    nlohmann::json response = {{"translated", res_by_request[i]->data}};
    std::string payload = response.dump();

    uint32_t cap =
        destination->GetSlotCapacity("entity_out", "entities_json", 2047);
    int ret = CopyToOperatorString(payload.c_str(), out->entities_json, cap,
                                   "entities_json", &diag_err);
    if (ret != 0) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status, diag_err.c_str(), "entities_json",
          options.converter_id.c_str(), static_cast<int>(i));
    }
  }

  if (written_count) *written_count = count;
  return COMPANY_ALG_SUCCESS;
}

OutputConverterDefinition MakeCAbiTranslationJsonOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "translate.json.cabi.v1";
  def.transport = "cabi";
  def.schema_id = "translate.json.response";
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
      NodePortDefinition("llm_answers", "TextBatch", true, "1:1")};
  def.encode_fn = &EncodeCAbiTranslationJson;
  return def;
}

OutputConverterDefinition MakeOperatorTranslationJsonOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "translate.json.operator.v1";
  def.transport = "operator";
  def.schema_id = "translate.json.response";
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
      NodePortDefinition("llm_answers", "TextBatch", true, "1:1")};
  def.encode_fn = &EncodeOperatorTranslationJson;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeCAbiTranslationJsonOutputConverter());
REGISTER_OUTPUT_CONVERTER(MakeOperatorTranslationJsonOutputConverter());

}  // namespace
}  // namespace llm_edgeflow

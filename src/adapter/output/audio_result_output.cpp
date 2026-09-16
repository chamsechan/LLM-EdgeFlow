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

int EncodeCAbiAudioResult(AlgContext* context,
                          const OutputPortBindings& bindings,
                          const OutputEncodeOptions& options,
                          ExternalOutputBatchView* destination,
                          size_t* written_count, AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Null AlgContext passed to Encode", "context",
        options.converter_id.c_str());
  }

  const auto* transcripts =
      context->Read<TextBatch>(bindings.GetActualKey("transcripts"));
  if (!transcripts) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: transcripts", "transcripts",
        options.converter_id.c_str());
  }

  const auto* intent_slots =
      context->Read<RuleMatchBatch>(bindings.GetActualKey("intent_slots"));
  if (!intent_slots) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Missing required context value: intent_slots", "intent_slots",
        options.converter_id.c_str());
  }

  const auto* raw_req_ids = context->Read<std::vector<uint64_t>>(
      bindings.GetActualKey("raw_request_ids"));
  if (!raw_req_ids) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: raw_request_ids",
        "raw_request_ids", options.converter_id.c_str());
  }

  int count = static_cast<int>(transcripts->size());
  int cap = static_cast<int>(destination->capacity > 0 ? destination->capacity
                                                       : destination->count);
  int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
      destination->items, &cap, count, options.converter_id.c_str(), status);
  if (valid_ret != 0) return valid_ret;

  std::vector<const TextBatch::value_type*> transcripts_by_request;
  if (!IndexResults(transcripts, raw_req_ids, &transcripts_by_request,
                    "transcripts", options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }
  std::vector<const RuleMatchBatch::value_type*> intent_slots_by_request;
  if (!IndexResults(intent_slots, raw_req_ids, &intent_slots_by_request,
                    "intent_slots", options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  for (int i = 0; i < count; ++i) {
    auto* out_ptr = destination->GetCAbi<CompanyAudioOutputStruct>(i);
    out_ptr->request_id = (*raw_req_ids)[i];
    out_ptr->status_code = intent_slots_by_request[i]->data.status_code;

    const std::string& slot_json =
        intent_slots_by_request[i]->data.match_result_json;

    if (!AdapterValidationHelper::CheckedStringCopy(
            out_ptr->transcribed_text, sizeof(out_ptr->transcribed_text),
            transcripts_by_request[i]->data.c_str(),
            "outputs[i].transcribed_text", i, options.converter_id.c_str(),
            status)) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }

    if (!AdapterValidationHelper::CheckedStringCopy(
            out_ptr->intent_slot_json, sizeof(out_ptr->intent_slot_json),
            slot_json.c_str(), "outputs[i].intent_slot_json", i,
            options.converter_id.c_str(), status)) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
  }

  if (written_count) *written_count = static_cast<size_t>(count);
  return COMPANY_ALG_SUCCESS;
}

int EncodeOperatorAudioResult(AlgContext* context,
                              const OutputPortBindings& bindings,
                              const OutputEncodeOptions& options,
                              ExternalOutputBatchView* destination,
                              size_t* written_count, AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Null AlgContext passed to Encode", "context",
        options.converter_id.c_str());
  }

  const auto* transcripts =
      context->Read<TextBatch>(bindings.GetActualKey("transcripts"));
  if (!transcripts) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: transcripts", "transcripts",
        options.converter_id.c_str());
  }

  const auto* intent_slots =
      context->Read<RuleMatchBatch>(bindings.GetActualKey("intent_slots"));
  if (!intent_slots) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Missing required context value: intent_slots", "intent_slots",
        options.converter_id.c_str());
  }

  const auto* raw_req_ids = context->Read<std::vector<uint64_t>>(
      bindings.GetActualKey("raw_request_ids"));
  if (!raw_req_ids) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: raw_request_ids",
        "raw_request_ids", options.converter_id.c_str());
  }

  size_t count = transcripts->size();
  if (destination->count < count) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Destination item count is less than output count",
        "destination", options.converter_id.c_str());
  }

  std::vector<const TextBatch::value_type*> transcripts_by_request;
  if (!IndexResults(transcripts, raw_req_ids, &transcripts_by_request,
                    "transcripts", options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }
  std::vector<const RuleMatchBatch::value_type*> intent_slots_by_request;
  if (!IndexResults(intent_slots, raw_req_ids, &intent_slots_by_request,
                    "intent_slots", options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  for (size_t i = 0; i < count; ++i) {
    auto* out =
        destination->GetSlot<CompanyOperatorAudioOutput>("audio_out", i);
    if (!out) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status, "Missing audio_out slot item", "audio_out",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    out->request_id = (*raw_req_ids)[i];
    out->status_code = intent_slots_by_request[i]->data.status_code;

    const std::string& slot_json =
        intent_slots_by_request[i]->data.match_result_json;

    std::string err;
    int ret = CopyToOperatorString(
        transcripts_by_request[i]->data.c_str(), out->transcribed_text,
        destination->GetSlotCapacity("audio_out", "transcribed_text", 511),
        "transcribed_text", &err);
    if (ret != 0) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status,
          err.empty() ? "Buffer too small for transcribed_text" : err.c_str(),
          "transcribed_text", options.converter_id.c_str(),
          static_cast<int>(i));
    }

    ret = CopyToOperatorString(
        slot_json.c_str(), out->intent_slot_json,
        destination->GetSlotCapacity("audio_out", "intent_slot_json", 1023),
        "intent_slot_json", &err);
    if (ret != 0) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status,
          err.empty() ? "Buffer too small for intent_slot_json" : err.c_str(),
          "intent_slot_json", options.converter_id.c_str(),
          static_cast<int>(i));
    }
  }

  if (written_count) *written_count = count;
  return COMPANY_ALG_SUCCESS;
}

OutputConverterDefinition MakeCAbiAudioResultOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "audio_result.plain.cabi.v1";
  def.transport = "cabi";
  def.schema_id = "audio_result.plain.response";
  def.schema_version = 1;
  def.external_type = "CompanyAudioOutputStruct";
  def.cardinality = "1:1";
  def.max_batch_size = 64;
  def.capacity_policy = "reject_overflow";
  def.thread_model = "stateless";
  def.external_slots = {{"outputs",
                         "CompanyAudioOutputStruct",
                         PortDirection::kOutput,
                         true,
                         "CompanyAudioOutputStruct",
                         "",
                         {"transcribed_text", "intent_slot_json"}}};
  def.logical_ports = {
      NodePortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      NodePortDefinition("transcripts", "TextBatch", true, "1:1"),
      NodePortDefinition("intent_slots", "RuleMatchBatch", true, "1:1")};
  def.encode_fn = &EncodeCAbiAudioResult;
  return def;
}

OutputConverterDefinition MakeOperatorAudioResultOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "audio_result.plain.operator.v1";
  def.transport = "operator";
  def.schema_id = "audio_result.plain.response";
  def.schema_version = 1;
  def.external_type = "CompanyOperatorAudioOutput";
  def.cardinality = "1:1";
  def.max_batch_size = 64;
  def.capacity_policy = "reject_overflow";
  def.thread_model = "stateless";
  def.external_slots = {{"audio_out",
                         "CompanyOperatorAudioOutput",
                         PortDirection::kOutput,
                         true,
                         "CompanyOperatorAudioOutput",
                         "audio_out",
                         {"transcribed_text", "intent_slot_json"}}};
  def.logical_ports = {
      NodePortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      NodePortDefinition("transcripts", "TextBatch", true, "1:1"),
      NodePortDefinition("intent_slots", "RuleMatchBatch", true, "1:1")};
  def.encode_fn = &EncodeOperatorAudioResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeCAbiAudioResultOutputConverter());
REGISTER_OUTPUT_CONVERTER(MakeOperatorAudioResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow

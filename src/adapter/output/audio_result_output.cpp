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

int EncodeOperatorAudioResult(AlgContext* context,
                              const OutputPortBindings& bindings,
                              const OutputEncodeOptions& options,
                              ExternalOutputBatchView* destination,
                              size_t* written_count, AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Encode", "context",
        options.converter_id.c_str());
  }

  const auto* transcripts =
      ReadOutputValue(*context, bindings, kTranscripts, options, status);
  if (!transcripts) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* intent_slots =
      ReadOutputValue(*context, bindings, kIntentSlots, options, status);
  if (!intent_slots) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* raw_req_ids =
      ReadOutputValue(*context, bindings, kRawRequestIds, options, status);
  if (!raw_req_ids) return COMPANY_ALG_ERR_INVALID_INPUT;

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

    if (!WriteOutputString(*destination, "audio_out", out->transcribed_text,
                           "transcribed_text",
                           transcripts_by_request[i]->data.c_str(), options,
                           status, i)) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }

    if (!WriteOutputString(*destination, "audio_out", out->intent_slot_json,
                           "intent_slot_json", slot_json.c_str(), options,
                           status, i)) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
  }

  if (written_count) *written_count = count;
  return COMPANY_ALG_SUCCESS;
}

OutputConverterDefinition MakeOperatorAudioResultOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "audio_result.plain.operator.v1";

  def.schema_id = "audio_result.plain.response";
  def.external_type = "CompanyOperatorAudioOutput";
  def.max_batch_size = 64;

  def.external_slots = {ExternalOutputSlot<CompanyOperatorAudioOutput>(
      "audio_out", {"transcribed_text", "intent_slot_json"})};
  def.logical_ports = {RequiredInputPort(kRawRequestIds),
                       RequiredInputPort(kTranscripts),
                       RequiredInputPort(kIntentSlots)};
  def.encode_fn = &EncodeOperatorAudioResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorAudioResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow

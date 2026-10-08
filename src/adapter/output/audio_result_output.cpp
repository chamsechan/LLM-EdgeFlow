#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/output/rule_match_response.h"
#include "adapter/result_validation.h"
#include "contracts/inference_payloads.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kOutputSlot = "audio_out";

int EncodeOperatorAudioResult(AlgContext* context,
                              const OutputEncodeOptions& options,
                              ExternalOutputBatchView* destination,
                              size_t* written_count, AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Encode", "context",
        options.Label().c_str());
  }

  const auto* transcripts =
      ReadOutputValue(*context, kTranscripts, options, status);
  if (!transcripts) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* intent_slots =
      ReadOutputValue(*context, kIntentSlots, options, status);
  if (!intent_slots) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* raw_req_ids = RequestIds(options, status);
  if (!raw_req_ids) return COMPANY_ALG_ERR_INVALID_INPUT;

  size_t count = transcripts->size();
  if (destination->count < count) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Destination item count is less than output count",
        "destination", options.Label().c_str());
  }

  std::vector<const TextBatch::value_type*> transcripts_by_request;
  if (!IndexResults(transcripts, raw_req_ids, &transcripts_by_request,
                    "transcripts", options.Label().c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }
  std::vector<const RuleMatchBatch::value_type*> intent_slots_by_request;
  if (!IndexResults(intent_slots, raw_req_ids, &intent_slots_by_request,
                    "intent_slots", options.Label().c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  for (size_t i = 0; i < count; ++i) {
    auto* out =
        destination->GetSlot<CompanyOperatorAudioOutput>(kOutputSlot, i);
    if (!out) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status, "Missing audio_out slot item", kOutputSlot,
          options.Label().c_str(), static_cast<int>(i));
    }

    out->request_id = (*raw_req_ids)[i];
    out->status_code = intent_slots_by_request[i]->data.status_code;

    const std::string slot_json =
        SerializeRuleMatchResponse(intent_slots_by_request[i]->data);

    if (!WriteOutputString(*destination, kOutputSlot, out->transcribed_text,
                           "transcribed_text", transcripts_by_request[i]->data,
                           options, status, i)) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }

    if (!WriteOutputString(*destination, kOutputSlot, out->intent_slot_json,
                           "intent_slot_json", slot_json, options, status, i)) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
  }

  if (written_count) *written_count = count;
  return COMPANY_ALG_SUCCESS;
}

// 输出字符串字段的尺寸参数；默认值按该业务的载荷设定，上限由平台结构登记决定。
struct Params {
  int64_t transcribed_text_max_bytes{};
  int64_t intent_slot_json_max_bytes{};
};

auto ParamSpec() {
  return Parameters<Params>(
      {MaxBytes("transcribed_text", &Params::transcribed_text_max_bytes)
           .Default(511),
       MaxBytes("intent_slot_json", &Params::intent_slot_json_max_bytes)
           .Default(1023)});
}

OutputConverterDefinition MakeOperatorAudioResultOutputConverter() {
  OutputConverterDefinition def;
  def.type = kOutputSlot;
  def.name = "audio_asr_intent";
  def.service_type =
      COMPANY_MOCK_SERVICE_AUDIO_ASR_INTENT;  // 占位取值，进内网核对
  def.slot = ExternalOutputSlot<CompanyOperatorAudioOutput>(kOutputSlot);
  def.logical_ports = {RequiredInputPort(kTranscripts),
                       RequiredInputPort(kIntentSlots)};
  def.params = ParamSpec();
  def.encode_fn = &EncodeOperatorAudioResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorAudioResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow

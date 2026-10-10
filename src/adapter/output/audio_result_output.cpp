#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_values.h"
#include "adapter/output/rule_match_response.h"
#include "adapter/result_validation.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"

namespace llm_edgeflow {
namespace {

constexpr auto kTranscribedText =
    MakeBlackboardKey<TextBatch>("transcribed_text");
constexpr auto kIntentSlot = MakeBlackboardKey<RuleMatchBatch>("intent_slot");

constexpr const char* kOutputSlot = "audio_out";

struct Params {
  int64_t transcribed_text_max_bytes = 0;
  int64_t intent_slot_json_max_bytes = 0;
};

Parameters<Params> ParamSpec() {
  return Parameters<Params>({
      MaxBytes("transcribed_text", &Params::transcribed_text_max_bytes)
          .Default(511),
      MaxBytes("intent_slot_json", &Params::intent_slot_json_max_bytes)
          .Default(1023),
  });
}

int EncodeOperatorAudioResult(AlgContext* context,
                              const OutputEncodeOptions& options,
                              ExternalOutputBatchView* destination,
                              size_t* written_count, AdapterStatus* status) {
  if (written_count) *written_count = 0;
  if (!context) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Encode", "context",
        options.Label().c_str());
  }

  const auto* transcripts =
      ReadOutputValue(*context, kTranscribedText, options, status);
  if (!transcripts) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* intent_slots =
      ReadOutputValue(*context, kIntentSlot, options, status);
  if (!intent_slots) return COMPANY_ALG_ERR_INVALID_INPUT;

  size_t count = transcripts->size();
  if (!destination || destination->count < count) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Destination item count is less than output count",
        "destination", options.Label().c_str());
  }

  std::vector<const TextBatch::value_type*> transcripts_by_request;
  if (!IndexResults(transcripts, destination->count, &transcripts_by_request,
                    "transcripts", options.Label().c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }
  std::vector<const RuleMatchBatch::value_type*> intent_slots_by_request;
  if (!IndexResults(intent_slots, destination->count, &intent_slots_by_request,
                    "intent_slots", options.Label().c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  size_t written = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!destination->HasSlot(kOutputSlot, i) && !destination->required) {
      continue;
    }
    AudioOutputValue out;

    out.status_code = intent_slots_by_request[i]->data.status_code;

    const std::string slot_json =
        SerializeRuleMatchResponse(intent_slots_by_request[i]->data);

    out.transcribed_text = transcripts_by_request[i]->data;
    out.intent_slot_json = slot_json;
    if (!WriteOutputValue(*destination, kOutputSlot, i, out, options, status)) {
      return status ? status->Code() : COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
    ++written;
  }

  if (written_count) *written_count = written;
  return COMPANY_ALG_SUCCESS;
}

OutputConverterDefinition MakeOperatorAudioResultOutputConverter() {
  OutputConverterDefinition def;
  def.type = kOutputSlot;
  def.name = "audio_asr_intent";
  def.slot = ExternalOutputSlot<AudioOutputValue>(kOutputSlot);
  def.logical_ports = {RequiredInputPort(kTranscribedText),
                       RequiredInputPort(kIntentSlot)};
  def.params = ParamSpec();
  def.encode_fn = &EncodeOperatorAudioResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorAudioResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow

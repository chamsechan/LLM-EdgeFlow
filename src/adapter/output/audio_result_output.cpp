#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_values.h"
#include "adapter/json_output_parameters.h"
#include "adapter/output/rule_match_response.h"
#include "adapter/result_validation.h"
#include "contracts/error_codes.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"
#include "nlohmann/json.hpp"

namespace llm_edgeflow {
namespace {

constexpr auto kTranscribedText =
    MakeBlackboardKey<TextBatch>("transcribed_text");
constexpr auto kIntentSlot = MakeBlackboardKey<RuleMatchBatch>("intent_slot");

constexpr const char* kOutputSlot = "json_out";

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
  if (!transcripts) return error::kInvalidInput;

  const auto* intent_slots =
      ReadOutputValue(*context, kIntentSlot, options, status);
  if (!intent_slots) return error::kInvalidInput;

  size_t count = transcripts->size();
  if (!destination || destination->count < count) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Destination item count is less than output count",
        "destination", options.Label().c_str());
  }

  std::vector<const TextBatch::value_type*> transcripts_by_request;
  if (!IndexResults(transcripts, destination->count, &transcripts_by_request,
                    "transcripts", options.Label().c_str(), status)) {
    return error::kInvalidInput;
  }
  std::vector<const RuleMatchBatch::value_type*> intent_slots_by_request;
  if (!IndexResults(intent_slots, destination->count, &intent_slots_by_request,
                    "intent_slots", options.Label().c_str(), status)) {
    return error::kInvalidInput;
  }

  size_t written = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!destination->HasSlot(kOutputSlot, i) && !destination->required) {
      continue;
    }
    JsonOutputValue out;

    out.result_json =
        nlohmann::json({{"transcribed_text", transcripts_by_request[i]->data},
                        {"intent_slot", BuildRuleMatchResponse(
                                            intent_slots_by_request[i]->data)}})
            .dump();
    if (!WriteOutputValue(*destination, kOutputSlot, i, out, options, status)) {
      return status ? status->Code() : error::kInvalidOutput;
    }
    ++written;
  }

  if (written_count) *written_count = written;
  return error::kOk;
}

OutputConverterDefinition MakeOperatorAudioResultOutputConverter() {
  OutputConverterDefinition def;
  def.type = kOutputSlot;
  def.name = "audio_asr_intent";
  def.slot = ExternalOutputSlot<JsonOutputValue>();
  def.logical_ports = {RequiredInputPort(kTranscribedText),
                       RequiredInputPort(kIntentSlot)};
  def.params = JsonOutputParameters();
  def.encode_fn = &EncodeOperatorAudioResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorAudioResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow

#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/converter_authoring.h"
#include "adapter/input_limits.h"
#include "adapter/io_converter.h"
#include "adapter/io_values.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"

namespace llm_edgeflow {
namespace {

constexpr auto kAudio = MakeBlackboardKey<AudioPcmBatch>("audio");

constexpr const char* kInputSlot = "audio_in";

AdapterStatus DecodeAudio(const AudioInputValue& input,
                          AudioPcmPayload* audio) {
  if (input.sample_rate < input_limits::kMinSampleRate ||
      input.sample_rate > input_limits::kMaxSampleRate) {
    return AdapterStatus::InvalidInput("sample_rate out of range",
                                       "audio_in.sample_rate");
  }
  audio->pcm_data = input.pcm;
  audio->sample_rate = input.sample_rate;
  return AdapterStatus::Ok();
}

int DecodeOperatorAudioInput(const ExternalInputBatchView& source,
                             const InputDecodeOptions& options,
                             AlgContext* context, AdapterStatus* status) {
  return DecodeRequestRows<AudioInputValue>(source, options, context, status,
                                            kInputSlot, kAudio, &DecodeAudio);
}

InputConverterDefinition MakeOperatorAudioInputConverter() {
  InputConverterDefinition def;
  def.type = kInputSlot;
  def.name = "audio_asr_intent";
  def.slot = ExternalInputSlot<AudioInputValue>(kInputSlot);
  def.logical_ports = {OutputPort(kAudio)};
  def.decode_fn = &DecodeOperatorAudioInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorAudioInputConverter());

}  // namespace
}  // namespace llm_edgeflow

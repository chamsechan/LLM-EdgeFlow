#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/biz_input_constraints.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "contracts/inference_payloads.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kInputSlot = "audio_in";

AdapterStatus DecodeAudio(const CompanyOperatorAudioInput& input,
                          AudioPcmPayload* audio) {
  if (input.sample_rate < biz_input::kMinSampleRate ||
      input.sample_rate > biz_input::kMaxSampleRate) {
    return AdapterStatus::InvalidInput("sample_rate out of range",
                                       "audio_in.sample_rate");
  }
  if (input.pcm_length < 0 ||
      input.pcm_length > biz_input::kMaxAudioPcmSamples ||
      static_cast<size_t>(input.pcm_length) >
          biz_input::kMaxAudioPcmBytes / sizeof(float)) {
    return AdapterStatus::InvalidInput("pcm_length invalid or exceeds limit",
                                       "audio_in.pcm_length");
  }
  if (input.pcm_length > 0 && !input.pcm_buffer) {
    return AdapterStatus::InvalidInput("pcm_buffer pointer is null",
                                       "audio_in.pcm_buffer");
  }
  if (input.pcm_length > 0) {
    audio->pcm_data.assign(input.pcm_buffer,
                           input.pcm_buffer + input.pcm_length);
  }
  audio->sample_rate = input.sample_rate;
  return AdapterStatus::Ok();
}

int DecodeOperatorAudioInput(const ExternalInputBatchView& source,
                             const InputDecodeOptions& options,
                             AlgContext* context, AdapterStatus* status) {
  return DecodeRequestRows<CompanyOperatorAudioInput>(
      source, options, context, status, kInputSlot, kAudioInputs, &DecodeAudio);
}

InputConverterDefinition MakeOperatorAudioInputConverter() {
  InputConverterDefinition def;
  def.converter_id = "audio.pcm";
  def.external_slots = {
      ExternalInputSlot<CompanyOperatorAudioInput>(kInputSlot)};
  def.logical_ports = {OutputPort(kAudioInputs)};
  def.decode_fn = &DecodeOperatorAudioInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorAudioInputConverter());

}  // namespace
}  // namespace llm_edgeflow

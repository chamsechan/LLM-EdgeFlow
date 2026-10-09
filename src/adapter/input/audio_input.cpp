#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/converter_authoring.h"
#include "adapter/input_limits.h"
#include "adapter/io_converter.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

constexpr auto kAudio = MakeBlackboardKey<AudioPcmBatch>("audio");

constexpr const char* kInputSlot = "audio_in";

AdapterStatus DecodeAudio(const CompanyOperatorAudioInput& input,
                          AudioPcmPayload* audio) {
  if (input.sample_rate < input_limits::kMinSampleRate ||
      input.sample_rate > input_limits::kMaxSampleRate) {
    return AdapterStatus::InvalidInput("sample_rate out of range",
                                       "audio_in.sample_rate");
  }
  if (input.pcm_length < 0 ||
      input.pcm_length > input_limits::kMaxAudioPcmSamples ||
      static_cast<size_t>(input.pcm_length) >
          input_limits::kMaxAudioPcmBytes / sizeof(float)) {
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
      source, options, context, status, kInputSlot, kAudio, &DecodeAudio);
}

InputConverterDefinition MakeOperatorAudioInputConverter() {
  InputConverterDefinition def;
  def.type = kInputSlot;
  def.name = "audio_asr_intent";
  def.service_type = kMockServiceAudioAsrIntent;
  def.slot = ExternalInputSlot<CompanyOperatorAudioInput>(kInputSlot);
  def.logical_ports = {OutputPort(kAudio)};
  def.decode_fn = &DecodeOperatorAudioInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorAudioInputConverter());

}  // namespace
}  // namespace llm_edgeflow

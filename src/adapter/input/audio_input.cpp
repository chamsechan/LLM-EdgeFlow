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

int DecodeOperatorAudioInput(const ExternalInputBatchView& source,
                             const InputDecodeOptions& options,
                             const InputPortBindings& bindings,
                             AlgContext* context, AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Decode", "context",
        options.converter_id.c_str());
  }
  if (source.count == 0 || source.count > 64) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Batch size out of range [1, 64]", "slots",
        options.converter_id.c_str());
  }

  std::vector<uint64_t> raw_req_ids;
  AudioPcmBatch raw_audios;

  raw_req_ids.reserve(source.count);
  raw_audios.reserve(source.count);

  for (size_t i = 0; i < source.count; ++i) {
    const auto* in = source.GetSlot<CompanyOperatorAudioInput>("audio_in", i);
    if (!in) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Missing audio_in input slot or slot item is null",
          "audio_in", options.converter_id.c_str(), static_cast<int>(i));
    }

    if (in->sample_rate < biz_input::kMinSampleRate ||
        in->sample_rate > biz_input::kMaxSampleRate) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "sample_rate out of range", "audio_in.sample_rate",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    if (in->pcm_length < 0 || in->pcm_length > biz_input::kMaxAudioPcmSamples ||
        static_cast<size_t>(in->pcm_length) >
            biz_input::kMaxAudioPcmBytes / sizeof(float)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "pcm_length invalid or exceeds limit", "audio_in.pcm_length",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    if (in->pcm_length > 0 && !in->pcm_buffer) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "pcm_buffer pointer is null", "audio_in.pcm_buffer",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    raw_req_ids.push_back(in->request_id);

    AudioPcmPayload pcm_dto;
    if (in->pcm_buffer && in->pcm_length > 0) {
      pcm_dto.pcm_data.assign(in->pcm_buffer, in->pcm_buffer + in->pcm_length);
    }
    pcm_dto.sample_rate = in->sample_rate;
    raw_audios.emplace_back(static_cast<uint32_t>(i), 0, std::move(pcm_dto));
  }

  if (!AdapterValidationHelper::PublishContextValue(
          *context, bindings.Key<std::vector<uint64_t>>("raw_request_ids"),
          std::move(raw_req_ids), options.converter_id.c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.Key<AudioPcmBatch>("audio_inputs"),
          std::move(raw_audios), options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  return COMPANY_ALG_SUCCESS;
}

InputConverterDefinition MakeOperatorAudioInputConverter() {
  InputConverterDefinition def;
  def.converter_id = "audio.pcm.operator.v1";

  def.schema_id = "audio.pcm.request";
  def.schema_version = 1;
  def.external_type = "CompanyOperatorAudioInput";
  def.max_batch_size = 64;

  def.external_slots = {{"audio_in",
                         "CompanyOperatorAudioInput",
                         PortDirection::kInput,
                         true,
                         "CompanyOperatorAudioInput",
                         "audio_in",
                         {}}};
  def.logical_ports = {OutputPort(kRawRequestIds), OutputPort(kAudioInputs)};
  def.decode_fn = &DecodeOperatorAudioInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorAudioInputConverter());

}  // namespace
}  // namespace llm_edgeflow

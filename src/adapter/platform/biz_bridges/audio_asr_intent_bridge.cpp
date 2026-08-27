#include "adapter/platform/platform_biz_bridge_registry.h"

namespace alg_framework {

void RegisterAudioAsrIntentBridge(PlatformBizBridgeRegistry& reg) {
  PlatformBizBridgeDescriptor desc;
  desc.biz_type = ALG_BIZ_TYPE_AUDIO_ASR_INTENT;
  desc.biz_name = "AudioAsrIntent";
  desc.internal_input_type_name = "CompanyAudioInputStruct";
  desc.internal_output_type_name = "CompanyAudioOutputStruct";
  desc.registration_identity = "builtin.audio_asr_intent";

  PlatformBizSlot in_slot;
  in_slot.logical_name = "audio_in";
  in_slot.type_suffix = "audio_in";
  in_slot.direction = IoDirection::kInput;
  in_slot.required = true;
  desc.input_slots.push_back(in_slot);

  PlatformBizSlot out_slot;
  out_slot.logical_name = "audio_out";
  out_slot.type_suffix = "audio_out";
  out_slot.direction = IoDirection::kOutput;
  out_slot.required = true;
  desc.output_slots.push_back(out_slot);

  desc.convert_sample_input =
      [](const std::unordered_map<std::string, const void*>& slots,
         ProcessLocalShadowStorage& storage, const void** out_internal_dto,
         std::string* err) -> int {
    auto it = slots.find("audio_in");
    if (it == slots.end() || !it->second) {
      if (err) *err = "Missing required input slot audio_in";
      return -3;
    }
    const auto* in = static_cast<const CompanyPlatformAudioInput*>(it->second);
    auto* dto = storage.AllocateShadowDto<CompanyAudioInputStruct>();
    dto->request_id = in->request_id;
    dto->pcm_length = in->pcm_length;
    dto->sample_rate = in->sample_rate;
    if (in->pcm_length > 0 && in->pcm_buffer) {
      storage.float_vectors.emplace_back(in->pcm_buffer,
                                         in->pcm_buffer + in->pcm_length);
      dto->pcm_buffer = storage.float_vectors.back().data();
    } else {
      dto->pcm_buffer = nullptr;
    }
    *out_internal_dto = dto;
    return 0;
  };

  desc.convert_sample_output =
      [](const void* internal_dto, void* external_output_struct,
         const ResolvedOutputPoolSpec& spec, std::string* err) -> int {
    if (!internal_dto || !external_output_struct) {
      if (err) *err = "Null internal DTO or external output struct pointer";
      return -4;
    }
    const auto* in_dto =
        static_cast<const CompanyAudioOutputStruct*>(internal_dto);
    auto* out =
        static_cast<CompanyPlatformAudioOutput*>(external_output_struct);
    out->request_id = in_dto->request_id;
    out->status_code = in_dto->status_code;

    int ret = PlatformBizBridgeRegistry::CopyToPooledString(
        in_dto->transcribed_text, out->transcribed_text,
        spec.GetCapacity("transcribed_text", 511), "transcribed_text", err);
    if (ret != 0) return ret;

    return PlatformBizBridgeRegistry::CopyToPooledString(
        in_dto->intent_slot_json, out->intent_slot_json,
        spec.GetCapacity("intent_slot_json", 1023), "intent_slot_json", err);
  };

  desc.create_shadow_output_dto = [](ProcessLocalShadowStorage& s) -> void* {
    return s.AllocateShadowDto<CompanyAudioOutputStruct>();
  };

  reg.RegisterBridge(desc);
}

REGISTER_PLATFORM_BIZ_BRIDGE(RegisterAudioAsrIntentBridge);

}  // namespace alg_framework

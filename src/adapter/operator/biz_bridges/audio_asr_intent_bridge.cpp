#include "adapter/biz_results.h"
#include "adapter/operator/operator_biz_bridge_registry.h"

namespace llm_edgeflow {

void RegisterAudioAsrIntentBridge(OperatorBizBridgeRegistry& reg) {
  auto desc = MakeSingleSlotBizBridge<AudioResult>(
      ALG_BIZ_TYPE_AUDIO_ASR_INTENT, "AudioAsrIntent",
      "CompanyAudioInputStruct", "builtin.audio_asr_intent", "audio_in",
      "audio_out");

  desc.convert_sample_input =
      [](const std::unordered_map<std::string, const void*>& slots,
         ProcessLocalShadowStorage& storage, const void** out_internal_dto,
         std::string* err) -> int {
    auto it = slots.find("audio_in");
    if (it == slots.end() || !it->second) {
      if (err) *err = "Missing required input slot audio_in";
      return -3;
    }
    const auto* in = static_cast<const CompanyOperatorAudioInput*>(it->second);
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
    const auto* in_dto = static_cast<const AudioResult*>(internal_dto);
    auto* out =
        static_cast<CompanyOperatorAudioOutput*>(external_output_struct);
    out->request_id = in_dto->request_id;
    out->status_code = in_dto->status_code;

    int ret = OperatorBizBridgeRegistry::CopyToPooledString(
        in_dto->transcribed_text.c_str(), out->transcribed_text,
        spec.GetCapacity("transcribed_text"), "transcribed_text", err);
    if (ret != 0) return ret;

    return OperatorBizBridgeRegistry::CopyToPooledString(
        in_dto->intent_slot_json.c_str(), out->intent_slot_json,
        spec.GetCapacity("intent_slot_json"), "intent_slot_json", err);
  };

  reg.RegisterBridge(desc);
}

REGISTER_OPERATOR_BIZ_BRIDGE(RegisterAudioAsrIntentBridge);

}  // namespace llm_edgeflow

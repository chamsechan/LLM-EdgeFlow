#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/business_adapter_registry.h"
#include "business/audio_asr/audio_asr_dto.h"
#include "company_alg_interface.h"

namespace alg_framework {

class AudioAsrIntentAdapter : public IBusinessAdapter {
 public:
  CompanyAlgBizType BizType() const override {
    return ALG_BIZ_TYPE_AUDIO_ASR_INTENT;
  }

  const char* BizName() const override { return "AudioAsrIntent"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{
        ALG_BIZ_TYPE_AUDIO_ASR_INTENT, "AudioAsrIntent",           "2.0.0",
        "CompanyAudioInputStruct",     "CompanyAudioOutputStruct", 64};
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx) override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, BizName());
    if (valid_ret != 0 || !ctx) return -3;

    std::vector<uint64_t> raw_req_ids;
    std::vector<AudioInputDto> raw_audios;

    raw_req_ids.reserve(num_inputs);
    raw_audios.reserve(num_inputs);

    for (int i = 0; i < num_inputs; ++i) {
      auto* in_audio = static_cast<const CompanyAudioInputStruct*>(inputs[i]);
      raw_req_ids.push_back(in_audio->request_id);

      AudioInputDto pcm_dto;
      pcm_dto.request_id = in_audio->request_id;
      if (in_audio->pcm_buffer && in_audio->pcm_length > 0) {
        pcm_dto.pcm_data.assign(in_audio->pcm_buffer,
                                in_audio->pcm_buffer + in_audio->pcm_length);
      }
      pcm_dto.sample_rate =
          in_audio->sample_rate > 0 ? in_audio->sample_rate : 16000;
      raw_audios.push_back(std::move(pcm_dto));
    }

    ctx->Set("raw_request_ids", std::move(raw_req_ids));
    ctx->Set("raw_audio_inputs", std::move(raw_audios));
    return 0;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs) override {
    if (!ctx) return -4;

    auto* res = ctx->Get<std::vector<AudioAsrResult>>("audio_final_outputs");
    if (!res) return -4;

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName());
    if (valid_ret != 0) return valid_ret;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<CompanyAudioOutputStruct*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->status_code = (*res)[i].status_code;

      strncpy(out_ptr->transcribed_text, (*res)[i].transcribed_text.c_str(),
              sizeof(out_ptr->transcribed_text) - 1);
      out_ptr->transcribed_text[sizeof(out_ptr->transcribed_text) - 1] = '\0';

      strncpy(out_ptr->intent_slot_json, (*res)[i].intent_slot_json.c_str(),
              sizeof(out_ptr->intent_slot_json) - 1);
      out_ptr->intent_slot_json[sizeof(out_ptr->intent_slot_json) - 1] = '\0';
    }
    *num_outputs = count;
    return 0;
  }
};

REGISTER_BUSINESS_ADAPTER(AudioAsrIntentAdapter);

}  // namespace alg_framework

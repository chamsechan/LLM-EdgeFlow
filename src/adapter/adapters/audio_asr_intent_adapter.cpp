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
        ALG_BIZ_TYPE_AUDIO_ASR_INTENT,
        "AudioAsrIntent",
        "2.0.0",
        "CompanyAudioInputStruct",
        "CompanyAudioOutputStruct",
        64,
        OwnershipPolicy::kCopyIn,
        ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne};
    return desc;
  }

  bool ValidatePipelineBinding(
      const std::string& pipeline_biz_name) const override {
    return pipeline_biz_name.find("audio_asr") != std::string::npos ||
           pipeline_biz_name.find("speech") != std::string::npos ||
           pipeline_biz_name == "AudioAsrIntent";
  }

  int Unpack(const void** inputs, int num_inputs,
             AlgContext* ctx) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, BizName());
    if (valid_ret != 0 || !ctx) return COMPANY_ALG_ERR_INVALID_INPUT;

    std::vector<uint64_t> raw_req_ids;
    std::vector<AudioInputDto> raw_audios;

    raw_req_ids.reserve(num_inputs);
    raw_audios.reserve(num_inputs);

    for (int i = 0; i < num_inputs; ++i) {
      auto* in_audio = static_cast<const CompanyAudioInputStruct*>(inputs[i]);
      if (!in_audio) return COMPANY_ALG_ERR_INVALID_INPUT;

      // ADP-001, ADP-005: 严格校验音频指针、长度与采样率
      if (in_audio->pcm_length < 0 || in_audio->sample_rate <= 0) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }
      if (in_audio->pcm_length > 0 && !in_audio->pcm_buffer) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      raw_req_ids.push_back(in_audio->request_id);

      AudioInputDto pcm_dto;
      pcm_dto.request_id = in_audio->request_id;
      // ADP-002: COPY_IN 内存深拷贝，与调用方生命周期完全隔离
      if (in_audio->pcm_buffer && in_audio->pcm_length > 0) {
        pcm_dto.pcm_data.assign(in_audio->pcm_buffer,
                                in_audio->pcm_buffer + in_audio->pcm_length);
      }
      pcm_dto.sample_rate = in_audio->sample_rate;
      raw_audios.push_back(std::move(pcm_dto));
    }

    ctx->Set("raw_request_ids", std::move(raw_req_ids));
    ctx->Set("raw_audio_inputs", std::move(raw_audios));
    return COMPANY_ALG_SUCCESS;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs) const override {
    if (!ctx) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    auto* res = ctx->Get<std::vector<AudioAsrResult>>("audio_final_outputs");
    if (!res) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName());
    if (valid_ret != 0) return valid_ret;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<CompanyAudioOutputStruct*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->status_code = (*res)[i].status_code;

      AdapterValidationHelper::CheckedStringCopy(
          out_ptr->transcribed_text, sizeof(out_ptr->transcribed_text),
          (*res)[i].transcribed_text.c_str(), "outputs[i].transcribed_text", i,
          BizName());

      AdapterValidationHelper::CheckedStringCopy(
          out_ptr->intent_slot_json, sizeof(out_ptr->intent_slot_json),
          (*res)[i].intent_slot_json.c_str(), "outputs[i].intent_slot_json", i,
          BizName());
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BUSINESS_ADAPTER(AudioAsrIntentAdapter);

}  // namespace alg_framework

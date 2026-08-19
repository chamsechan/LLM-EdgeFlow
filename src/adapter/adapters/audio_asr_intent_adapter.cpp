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
        OutputCardinality::kOneToOne,
        {"speech_audio_asr_intent_slot"}};  // RECHECK-002: 精确白名单
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx,
             AdapterStatus* out_status = nullptr) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, BizName());
    if (valid_ret != 0 || !ctx) {
      if (out_status) {
        *out_status = AdapterStatus::InvalidInput(
            "Batch envelope validation failed or null AlgContext", "inputs", -1,
            BizName());
      }
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }

    std::vector<uint64_t> raw_req_ids;
    std::vector<AudioInputDto> raw_audios;

    raw_req_ids.reserve(num_inputs);
    raw_audios.reserve(num_inputs);

    // 最大允许单次音频采样点 (1分钟 @ 16kHz = 960,000 点, RECHECK-004)
    constexpr int64_t kMaxAudioSamples = 16000 * 60;

    for (int i = 0; i < num_inputs; ++i) {
      auto* in_audio = static_cast<const CompanyAudioInputStruct*>(inputs[i]);
      if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in_audio, i,
                                                   BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      // ADP-001, ADP-005, RECHECK-004: 严格校验采样率与音频采样点区间
      if (!AdapterValidationHelper::RequireRange(
              "inputs[i].sample_rate", in_audio->sample_rate, 8000, 192000, i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }
      if (!AdapterValidationHelper::RequireRange(
              "inputs[i].pcm_length", in_audio->pcm_length, 0, kMaxAudioSamples,
              i, BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      if (in_audio->pcm_length > 0) {
        if (!AdapterValidationHelper::RequireNotNull("inputs[i].pcm_buffer",
                                                     in_audio->pcm_buffer, i,
                                                     BizName(), out_status)) {
          return COMPANY_ALG_ERR_INVALID_INPUT;
        }
        // 乘法溢出与最大内存安全校验 (10MB 缓冲区上限)
        if (!AdapterValidationHelper::CheckedMultiply(
                "inputs[i].pcm_buffer", in_audio->pcm_length, sizeof(float),
                10 * 1024 * 1024, i, BizName(), out_status)) {
          return COMPANY_ALG_ERR_INVALID_INPUT;
        }
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

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs,
           AdapterStatus* out_status = nullptr) const override {
    if (!ctx) {
      if (out_status) {
        *out_status = AdapterStatus::BufferTooSmall(
            "Null AlgContext passed to Pack", "ctx", -1, BizName());
      }
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }

    auto* res = ctx->Get<std::vector<AudioAsrResult>>("audio_final_outputs");
    if (!res) {
      if (out_status) {
        *out_status = AdapterStatus::BufferTooSmall(
            "audio_final_outputs not found in AlgContext",
            "audio_final_outputs", -1, BizName());
      }
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName());
    if (valid_ret != 0) {
      if (out_status) {
        *out_status = AdapterStatus::BufferTooSmall(
            "Output slots insufficient or null", "outputs", -1, BizName());
      }
      return valid_ret;
    }

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<CompanyAudioOutputStruct*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->status_code = (*res)[i].status_code;

      // RECHECK-001: 校验 CheckedStringCopy 返回值，截断时严格拒绝返回 -4
      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->transcribed_text, sizeof(out_ptr->transcribed_text),
              (*res)[i].transcribed_text.c_str(), "outputs[i].transcribed_text",
              i, BizName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }

      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->intent_slot_json, sizeof(out_ptr->intent_slot_json),
              (*res)[i].intent_slot_json.c_str(), "outputs[i].intent_slot_json",
              i, BizName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BUSINESS_ADAPTER(AudioAsrIntentAdapter);

}  // namespace alg_framework

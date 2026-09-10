#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_adapter_registry.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/biz_input_constraints.h"
#include "adapter/biz_results.h"
#include "adapter/result_packing_adapter.h"
#include "adapter/result_validation.h"
#include "edgeflow/c_api.h"

namespace llm_edgeflow {

inline static constexpr char kAudioAsrBizName[] =
    "speech_audio_asr_intent_slot";

class AudioAsrIntentAdapter
    : public ResultPackingAdapter<AudioAsrIntentAdapter,
                                  CompanyAudioOutputStruct, AudioResult> {
 public:
  CompanyAlgBizType BizType() const override {
    return ALG_BIZ_TYPE_AUDIO_ASR_INTENT;
  }

  const char* AdapterName() const override { return "AudioAsrIntent"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{
        ALG_BIZ_TYPE_AUDIO_ASR_INTENT,
        "AudioAsrIntent",
        COMPANY_ALG_ABI_VERSION,
        "CompanyAudioInputStruct",
        "CompanyAudioOutputStruct",
        64,
        OwnershipPolicy::kCopyIn,
        ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne,
        {{kAudioAsrBizName,
          "audio_asr",
          "语音识别与意图槽位",
          {RequiredBizInput(kRawRequestIds), RequiredBizInput(kAudioInputs)},
          {BizOutput(kIntentSlots), BizOutput(kTranscripts)}}}};
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx,
             AdapterStatus* out_status = nullptr) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, AdapterName());
    if (valid_ret != 0 || !ctx) {
      return AdapterValidationHelper::ReturnInvalidInput(
          out_status, "Batch envelope validation failed or null AlgContext",
          "inputs", AdapterName());
    }

    std::vector<uint64_t> raw_req_ids;
    AudioPcmBatch raw_audios;

    raw_req_ids.reserve(num_inputs);
    raw_audios.reserve(num_inputs);

    for (int i = 0; i < num_inputs; ++i) {
      auto* in_audio = static_cast<const CompanyAudioInputStruct*>(inputs[i]);
      if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in_audio, i,
                                                   AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      if (!AdapterValidationHelper::RequireRange(
              "inputs[i].sample_rate", in_audio->sample_rate,
              biz_input::kMinSampleRate, biz_input::kMaxSampleRate, i,
              AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }
      if (!AdapterValidationHelper::RequireRange(
              "inputs[i].pcm_length", in_audio->pcm_length, 0,
              biz_input::kMaxAudioPcmSamples, i, AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      if (in_audio->pcm_length > 0) {
        if (!AdapterValidationHelper::RequireNotNull(
                "inputs[i].pcm_buffer", in_audio->pcm_buffer, i, AdapterName(),
                out_status)) {
          return COMPANY_ALG_ERR_INVALID_INPUT;
        }
        if (!AdapterValidationHelper::CheckedMultiply(
                "inputs[i].pcm_buffer", in_audio->pcm_length, sizeof(float),
                biz_input::kMaxAudioPcmBytes, i, AdapterName(), out_status)) {
          return COMPANY_ALG_ERR_INVALID_INPUT;
        }
      }

      raw_req_ids.push_back(in_audio->request_id);

      AudioPcmPayload pcm_dto;
      if (in_audio->pcm_buffer && in_audio->pcm_length > 0) {
        pcm_dto.pcm_data.assign(in_audio->pcm_buffer,
                                in_audio->pcm_buffer + in_audio->pcm_length);
      }
      pcm_dto.sample_rate = in_audio->sample_rate;
      raw_audios.emplace_back(static_cast<uint32_t>(i), 0, std::move(pcm_dto));
    }

    if (!AdapterValidationHelper::PublishContextValue(
            *ctx, kRawRequestIds, std::move(raw_req_ids), AdapterName(),
            out_status) ||
        !AdapterValidationHelper::PublishContextValue(
            *ctx, kAudioInputs, std::move(raw_audios), AdapterName(),
            out_status)) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    return COMPANY_ALG_SUCCESS;
  }

  template <typename Output>
  int PackTyped(AlgContext* ctx, void** outputs, int* num_outputs,
                AdapterStatus* out_status = nullptr) const {
    if (!ctx) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          out_status, "Null AlgContext passed to Pack", "ctx", AdapterName());
    }

    const auto* transcripts = AdapterValidationHelper::ReadRequiredContextValue(
        *ctx, kTranscripts, AdapterName(), out_status);
    if (!transcripts) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    const auto* intent_slots = ctx->Read(kIntentSlots);
    const auto* raw_req_ids = ctx->Read(kRawRequestIds);

    int count = static_cast<int>(transcripts->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, AdapterName(), out_status);
    if (valid_ret != 0) return valid_ret;

    std::vector<const TextBatch::value_type*> transcripts_by_request;
    if (!IndexResults(transcripts, raw_req_ids, &transcripts_by_request,
                      "transcripts", AdapterName(), out_status))
      return COMPANY_ALG_ERR_INVALID_INPUT;
    std::vector<const RuleMatchBatch::value_type*> intent_slots_by_request;
    if (!IndexResults(intent_slots, raw_req_ids, &intent_slots_by_request,
                      "intent_slots", AdapterName(), out_status))
      return COMPANY_ALG_ERR_INVALID_INPUT;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<Output*>(outputs[i]);
      uint64_t req_id =
          (raw_req_ids && i < static_cast<int>(raw_req_ids->size()))
              ? (*raw_req_ids)[i]
              : transcripts_by_request[i]->req_id;
      out_ptr->request_id = req_id;
      out_ptr->status_code = intent_slots_by_request[i]->data.status_code;

      std::string slot_json = "{}";
      if (intent_slots && i < static_cast<int>(intent_slots->size())) {
        slot_json = intent_slots_by_request[i]->data.match_result_json;
      }

      if (!CopyResultString(out_ptr->transcribed_text,
                            transcripts_by_request[i]->data.c_str(),
                            "outputs[i].transcribed_text", i, AdapterName(),
                            out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }

      if (!CopyResultString(out_ptr->intent_slot_json, slot_json.c_str(),
                            "outputs[i].intent_slot_json", i, AdapterName(),
                            out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BIZ_ADAPTER(AudioAsrIntentAdapter);

}  // namespace llm_edgeflow

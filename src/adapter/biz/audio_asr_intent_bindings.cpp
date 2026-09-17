#include "adapter/converter_authoring.h"
#include "adapter/io_binding.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

BizDefinition MakeAudioAsrIntentBizDefinition() {
  BizDefinition def;
  def.biz_name = "speech_audio_asr_intent_slot";
  def.demo_biz = "audio_asr";
  def.display_name = "语音识别与意图槽位";
  def.ingress = {
      BizPortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      BizPortDefinition("audio_inputs", "AudioPcmBatch", true, "1:1")};
  def.egress = {
      BizPortDefinition("transcripts", "TextBatch", true, "1:1"),
      BizPortDefinition("intent_slots", "RuleMatchBatch", true, "1:1")};
  return def;
}

const bool g_reg_audio_asr_intent_biz = []() {
  auto def = MakeAudioAsrIntentBizDefinition();
  if (!PipelineCatalog::FindBiz(def.biz_name)) {
    PipelineCatalog::RegisterBizDefinition(def);
  }
  return true;
}();

BizExposureDefinition MakeAudioAsrIntentBizExposure() {
  BizExposureDefinition def;
  def.biz_name = "speech_audio_asr_intent_slot";
  def.max_batch_size = 64;
  def.required_transports = {"operator"};
  return def;
}

IoBindingDefinition MakeAudioAsrIntentOperatorBinding() {
  IoBindingDefinition def;
  def.binding_id = "audio_asr_intent.operator.v1";
  def.biz_name = "speech_audio_asr_intent_slot";
  def.transport = "operator";
  def.input_converter_id = "audio.pcm.operator.v1";
  def.output_converter_id = "audio_result.plain.operator.v1";
  def.input_ports = {{"raw_request_ids", "raw_request_ids"},
                     {"audio_inputs", "audio_inputs"}};
  def.output_ports = {{"raw_request_ids", "raw_request_ids"},
                      {"transcripts", "transcripts"},
                      {"intent_slots", "intent_slots"}};
  def.max_batch_size = 64;
  return def;
}

REGISTER_BIZ_EXPOSURE(MakeAudioAsrIntentBizExposure());
REGISTER_IO_BINDING(MakeAudioAsrIntentOperatorBinding());

}  // namespace
}  // namespace llm_edgeflow

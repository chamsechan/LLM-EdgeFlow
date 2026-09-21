#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_binding.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kBizName = "speech_audio_asr_intent_slot";
constexpr size_t kMaxBatchSize = 64;

BizDefinition MakeAudioAsrIntentBizDefinition() {
  BizDefinition def;
  def.biz_name = kBizName;
  def.demo_biz = "audio_asr";
  def.display_name = "语音识别与意图槽位";
  def.ingress = {RequiredBizInput(kRawRequestIds),
                 RequiredBizInput(kAudioInputs)};
  def.egress = {BizOutput(kTranscripts), BizOutput(kIntentSlots)};
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
  def.biz_name = kBizName;
  def.max_batch_size = kMaxBatchSize;

  return def;
}

IoBindingDefinition MakeAudioAsrIntentOperatorBinding() {
  IoBindingDefinition def;
  def.binding_id = "audio_asr_intent.operator.v1";
  def.biz_name = kBizName;

  def.input_converter_id = "audio.pcm.operator.v1";
  def.output_converter_id = "audio_result.plain.operator.v1";
  def.input_ports = {BindIoPort(kRawRequestIds), BindIoPort(kAudioInputs)};
  def.output_ports = {BindIoPort(kRawRequestIds), BindIoPort(kTranscripts),
                      BindIoPort(kIntentSlots)};
  def.max_batch_size = kMaxBatchSize;
  return def;
}

REGISTER_BIZ_EXPOSURE(MakeAudioAsrIntentBizExposure());
REGISTER_IO_BINDING(MakeAudioAsrIntentOperatorBinding());

}  // namespace
}  // namespace llm_edgeflow

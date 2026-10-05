#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_binding.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kBizName = "audio_asr_intent";

BizDefinition MakeAudioAsrIntentBizDefinition() {
  BizDefinition def;
  def.biz_name = kBizName;
  def.display_name = "语音识别与意图槽位";
  def.ingress = {RequiredBizInput(kAudioInputs)};
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

IoBindingDefinition MakeAudioAsrIntentOperatorBinding() {
  IoBindingDefinition def;
  def.biz_name = kBizName;
  def.input_converter_id = "audio.pcm";
  def.output_converter_id = "audio_result.plain";
  return def;
}

REGISTER_IO_BINDING(MakeAudioAsrIntentOperatorBinding());

}  // namespace
}  // namespace llm_edgeflow

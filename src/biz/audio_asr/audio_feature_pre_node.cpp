#include "biz/audio_asr/audio_asr_contract.h"
#include "biz/audio_asr/audio_asr_dto.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"
#include "nodes/node_support.h"

namespace alg_framework {

class AudioFeaturePreNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "AudioFeaturePreNode";

  AudioFeaturePreNode() : NodeBase(kNodeType) {}

 protected:
  int ProcessNode(AlgContext& req_ctx) override {
    const auto* raw_audios = Require(req_ctx, kRawAudioInputs, -6101);
    const auto* req_ids = Require(req_ctx, kRawRequestIds, -6101);

    if (!raw_audios || !req_ids) {
      return -6101;
    }

    std::vector<TraceableItem<IAudioAsrEngine::AudioPcmData>> traceable_audios;
    for (size_t i = 0; i < raw_audios->size(); ++i) {
      IAudioAsrEngine::AudioPcmData pcm_engine;
      pcm_engine.pcm_data = (*raw_audios)[i].pcm_data;
      pcm_engine.sample_rate = (*raw_audios)[i].sample_rate;
      traceable_audios.emplace_back((*req_ids)[i], 0, std::move(pcm_engine));
    }

    Publish(req_ctx, kTraceableAudioItems, std::move(traceable_audios));
    return 0;
  }
};

NodeDefinition MakeAudioFeaturePreNodeDefinition() {
  NodeDefinition def;
  def.node_type = AudioFeaturePreNode::kNodeType;
  def.category = "biz";
  def.description = "Audio feature pre-processing node";
  def.inputs = {RequiredInput(kRawAudioInputs), RequiredInput(kRawRequestIds)};
  def.outputs = {Output(kTraceableAudioItems)};
  def.biz_names = {kAudioAsrBizName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(AudioFeaturePreNode,
                              MakeAudioFeaturePreNodeDefinition());

}  // namespace alg_framework

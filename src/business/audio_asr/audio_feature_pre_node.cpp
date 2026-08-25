#include "business/audio_asr/audio_asr_contract.h"
#include "business/audio_asr/audio_asr_dto.h"
#include "core/alg_context.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"

namespace alg_framework {

class AudioFeaturePreNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "AudioFeaturePreNode";

  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    (void)config;
    (void)session_ctx;
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* raw_audios = req_ctx->Get(kRawAudioInputs);
    auto* req_ids = req_ctx->Get(kRawRequestIds);

    if (!raw_audios || !req_ids) {
      req_ctx->SetError(-6101,
                        "AudioFeaturePreNode: Missing raw audio or req_ids");
      return -6101;
    }

    std::vector<TraceableItem<IAudioAsrEngine::AudioPcmData>> traceable_audios;
    for (size_t i = 0; i < raw_audios->size(); ++i) {
      IAudioAsrEngine::AudioPcmData pcm_engine;
      pcm_engine.pcm_data = (*raw_audios)[i].pcm_data;
      pcm_engine.sample_rate = (*raw_audios)[i].sample_rate;
      traceable_audios.emplace_back((*req_ids)[i], 0, std::move(pcm_engine));
    }

    req_ctx->Set(kTraceableAudioItems, std::move(traceable_audios));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = kNodeType;
    return name;
  }
};

NodeDefinition MakeAudioFeaturePreNodeDefinition() {
  NodeDefinition def;
  def.node_type = AudioFeaturePreNode::kNodeType;
  def.category = "business";
  def.description = "Audio feature pre-processing node";
  def.inputs = {RequiredInput(kRawAudioInputs), RequiredInput(kRawRequestIds)};
  def.outputs = {Output(kTraceableAudioItems)};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(AudioFeaturePreNode,
                              MakeAudioFeaturePreNodeDefinition());

}  // namespace alg_framework

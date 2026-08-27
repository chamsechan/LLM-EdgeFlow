#include <iostream>
#include <string>
#include <vector>

#include "biz/audio_asr/audio_asr_contract.h"
#include "biz/audio_asr/audio_asr_dto.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "nodes/node_support.h"

namespace alg_framework {

class AudioPostNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "AudioPostNode";

  AudioPostNode() : NodeBase(kNodeType) {}

 protected:
  int ProcessNode(AlgContext& req_ctx) override {
    const auto* req_ids = Require(req_ctx, kRawRequestIds, -6401);
    const auto* transcripts = Require(req_ctx, kAsrTranscripts, -6401);
    const auto* slot_jsons = Require(req_ctx, kIntentSlotResults, -6401);

    if (!req_ids || !transcripts || !slot_jsons) {
      return -6401;
    }

    std::vector<AudioAsrResult> outputs(req_ids->size());
    for (size_t i = 0; i < req_ids->size(); ++i) {
      outputs[i].request_id = (*req_ids)[i];
      outputs[i].status_code = 0;
      outputs[i].transcribed_text = (*transcripts)[i].data;
      outputs[i].intent_slot_json = (*slot_jsons)[i];
    }

    Publish(req_ctx, kAudioFinalOutputs, std::move(outputs));
    return 0;
  }
};

NodeDefinition MakeAudioPostNodeDefinition() {
  NodeDefinition def;
  def.node_type = AudioPostNode::kNodeType;
  def.category = "biz";
  def.description = "Audio ASR post-processing node";
  def.inputs = {RequiredInput(kRawRequestIds), RequiredInput(kAsrTranscripts),
                RequiredInput(kIntentSlotResults)};
  def.outputs = {Output(kAudioFinalOutputs)};
  def.biz_names = {kAudioAsrBizName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(AudioPostNode, MakeAudioPostNodeDefinition());

}  // namespace alg_framework

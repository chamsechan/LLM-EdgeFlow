#include <iostream>
#include <string>
#include <vector>

#include "biz/audio_asr/audio_asr_contract.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"
#include "nodes/traceable_unary_inference_node.h"

namespace alg_framework {

class AsrInferNode final
    : public TraceableUnaryInferenceNode<
          IAudioAsrEngine, IAudioAsrEngine::AudioPcmData, std::string> {
 public:
  inline static constexpr char kNodeType[] = "AsrInferNode";

  AsrInferNode()
      : TraceableUnaryInferenceNode(kNodeType, "asr_model_v1",
                                    kTraceableAudioItems, kAsrTranscripts,
                                    -6201) {}

 protected:
  int InferBatch(const InputBatch& input, OutputBatch* output) override {
    return engine()->InferTraceableBatch(input, output);
  }
};

NodeDefinition MakeAsrInferNodeDefinition() {
  NodeDefinition def;
  def.node_type = AsrInferNode::kNodeType;
  def.category = "biz";
  def.description = "Audio ASR speech-to-text inference node";
  def.inputs = {RequiredInput(kTraceableAudioItems)};
  def.outputs = {Output(kAsrTranscripts)};
  def.config_fields = {ConfigFieldDefinition{
      "bind_model", ConfigValueKind::kString, false, "asr_model_v1"}};
  def.model_capability = "asr";
  def.model_config_field = "bind_model";
  def.biz_names = {kAudioAsrBizName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(AsrInferNode, MakeAsrInferNodeDefinition());

}  // namespace alg_framework

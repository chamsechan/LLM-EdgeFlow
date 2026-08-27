#include <iostream>
#include <vector>

#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "engine/engine_interface.h"
#include "nodes/model_bound_node.h"

namespace alg_framework {

/**
 * @brief 语音识别 ASR 文本转写通用算子 (AsrTranscribeNode, 调用绑定的
 * IAudioAsrEngine)
 */
class AsrTranscribeNode final : public ModelBoundNode<IAudioAsrEngine> {
 public:
  inline static constexpr char kNodeType[] = "AsrTranscribeNode";

  AsrTranscribeNode()
      : ModelBoundNode<IAudioAsrEngine>(kNodeType, "mock_asr_model"),
        in_audio_("audio", "audio", "AudioPcmBatch"),
        out_text_("text", "text", "TextBatch") {}

 protected:
  bool InitModelNode(const nlohmann::json& /*config*/,
                     SessionContext& /*session_ctx*/) override {
    BindPort(in_audio_);
    BindPort(out_text_);
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* audio_items =
        in_audio_.Require(req_ctx, -7001, "AsrTranscribeNode audio input");
    if (!audio_items) {
      return -7001;
    }

    if (audio_items->empty()) {
      out_text_.Set(req_ctx, TextBatch{});
      return 0;
    }

    std::vector<TraceableItem<IAudioAsrEngine::AudioPcmData>> engine_items;
    engine_items.reserve(audio_items->size());
    for (const auto& item : *audio_items) {
      engine_items.emplace_back(item.req_id, item.sub_id,
                                IAudioAsrEngine::AudioPcmData{
                                    item.data.pcm_data, item.data.sample_rate});
    }

    TextBatch transcripts;
    std::cout << "[AsrTranscribeNode] Inferring transcripts for "
              << engine_items.size() << " audio streams..." << std::endl;

    int ret = engine()->InferTraceableBatch(engine_items, &transcripts);
    if (ret != 0) {
      return Fail(req_ctx, ret, "AsrTranscribeNode: ASR inference failed");
    }

    out_text_.Set(req_ctx, std::move(transcripts));
    return 0;
  }

 private:
  BoundInput<AudioPcmBatch> in_audio_;
  BoundOutput<TextBatch> out_text_;
};

NodeDefinition MakeAsrTranscribeNodeDefinition() {
  NodeDefinition def;
  def.node_type = AsrTranscribeNode::kNodeType;
  def.category = "common";
  def.description = "Audio speech recognition (ASR) transcription node";
  def.inputs = {RequiredInputPort(
      "audio", BlackboardKey<AudioPcmBatch>{"", "AudioPcmBatch"})};
  def.outputs = {OutputPort("text", BlackboardKey<TextBatch>{"", "TextBatch"})};
  def.config_fields = {ConfigFieldDefinition{
      "bind_model", ConfigValueKind::kString, false, "mock_asr_model"}};
  def.model_capability = "asr";
  def.model_config_field = "bind_model";
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(AsrTranscribeNode,
                              MakeAsrTranscribeNodeDefinition());

}  // namespace alg_framework

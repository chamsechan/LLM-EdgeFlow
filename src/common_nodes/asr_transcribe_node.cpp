#include "company_alg_log.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "engine/model_interface.h"
#include "nodes/model_bound_node.h"
#include "nodes/node_error_codes.h"
#include "nodes/traceable_batch_validation.h"

namespace alg_framework {

/**
 * @brief 语音识别 ASR 文本转写通用算子 (AsrTranscribeNode, 调用绑定的
 * IAsrModel)
 */
class AsrTranscribeNode final : public ModelBoundNode<IAsrModel> {
 public:
  inline static constexpr char kNodeType[] = "AsrTranscribeNode";

  AsrTranscribeNode()
      : ModelBoundNode<IAsrModel>(kNodeType),
        in_audio_("audio"),
        out_text_("text") {}

 protected:
  bool InitModelNode(const NodeInitContext& init_ctx,
                     const nlohmann::json& /*config*/,
                     SessionContext& /*session_ctx*/) override {
    BindPort(init_ctx, in_audio_);
    BindPort(init_ctx, out_text_);
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* audio_items =
        in_audio_.Require(req_ctx, node_error::asr_transcribe::kMissingInput,
                          "AsrTranscribeNode audio input");
    if (!audio_items) {
      return node_error::asr_transcribe::kMissingInput;
    }

    if (audio_items->empty()) {
      out_text_.Set(req_ctx, TextBatch{});
      return 0;
    }

    TextBatch transcripts;
    ALG_LOG_DEBUG(
        "[AsrTranscribeNode] Inferring transcripts for %zu audio "
        "streams...\n",
        audio_items->size());

    int ret = model()->Transcribe(*audio_items, &transcripts);
    if (ret != 0) {
      return Fail(req_ctx, ret, "AsrTranscribeNode: ASR inference failed");
    }

    const auto alignment =
        ValidatePreservedTraceableAlignment(*audio_items, transcripts);
    if (alignment.error == TraceableAlignmentError::kCountMismatch) {
      return Fail(req_ctx, node_error::asr_transcribe::kOutputCountMismatch,
                  "AsrTranscribeNode: transcript count mismatch");
    }
    if (alignment.error == TraceableAlignmentError::kProvenanceMismatch) {
      return Fail(req_ctx,
                  node_error::asr_transcribe::kOutputProvenanceMismatch,
                  "AsrTranscribeNode: transcript provenance mismatch");
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
      "audio", BlackboardKey<AudioPcmBatch>{"", "AudioPcmBatch"}, "1:1",
      "preserve", "request")};
  def.outputs = {OutputPort("text", BlackboardKey<TextBatch>{"", "TextBatch"},
                            "1:1", "preserve", "request")};
  def.config_fields = {ConfigFieldDefinition{
      "bind_model", ConfigValueKind::kString, false, "asr_model_v1"}};
  def.model_capability = "asr";
  def.model_config_field = "bind_model";
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(AsrTranscribeNode,
                              MakeAsrTranscribeNodeDefinition());

}  // namespace alg_framework

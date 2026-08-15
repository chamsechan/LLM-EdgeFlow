#include <iostream>
#include <string>
#include <vector>

#include "core/alg_context.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"

namespace alg_framework {

class AsrInferNode : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    std::string bind_model_id = config.value("bind_model", "asr_model_v1");
    asr_engine_ =
        session_ctx->GetModelManager().GetModel<IAudioAsrEngine>(bind_model_id);
    if (!asr_engine_) {
      std::cerr << "[AsrInferNode] Failed to get IAudioAsrEngine model: "
                << bind_model_id << std::endl;
      return false;
    }
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* audio_items =
        req_ctx->Get<std::vector<TraceableItem<IAudioAsrEngine::AudioPcmData>>>(
            "traceable_audio_items");
    if (!audio_items) {
      req_ctx->SetError(-6201, "AsrInferNode: Missing traceable_audio_items");
      return -6201;
    }

    std::vector<TraceableItem<std::string>> transcripts;
    int ret = asr_engine_->InferTraceableBatch(*audio_items, &transcripts);
    if (ret != 0) {
      req_ctx->SetError(ret, "AsrInferNode: ASR inference failed");
      return ret;
    }

    req_ctx->Set("asr_transcripts", std::move(transcripts));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "AsrInferNode";
    return name;
  }

 private:
  std::shared_ptr<IAudioAsrEngine> asr_engine_;
};

REGISTER_NODE(AsrInferNode);

}  // namespace alg_framework

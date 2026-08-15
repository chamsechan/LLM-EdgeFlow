#include <iostream>
#include <vector>

#include "core/alg_context.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"

namespace alg_framework {

class AudioFeaturePreNode : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    (void)config;
    (void)session_ctx;
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* raw_audios = req_ctx->Get<std::vector<IAudioAsrEngine::AudioPcmData>>(
        "raw_audio_inputs");
    auto* req_ids = req_ctx->Get<std::vector<uint64_t>>("raw_request_ids");

    if (!raw_audios || !req_ids) {
      req_ctx->SetError(-6101,
                        "AudioFeaturePreNode: Missing raw audio or req_ids");
      return -6101;
    }

    std::vector<TraceableItem<IAudioAsrEngine::AudioPcmData>> traceable_audios;
    for (size_t i = 0; i < raw_audios->size(); ++i) {
      traceable_audios.emplace_back((*req_ids)[i], 0, (*raw_audios)[i]);
    }

    req_ctx->Set("traceable_audio_items", std::move(traceable_audios));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "AudioFeaturePreNode";
    return name;
  }
};

REGISTER_NODE(AudioFeaturePreNode);

}  // namespace alg_framework

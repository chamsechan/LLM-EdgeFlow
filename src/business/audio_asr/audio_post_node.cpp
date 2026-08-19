#include <iostream>
#include <string>
#include <vector>

#include "business/audio_asr/audio_asr_dto.h"
#include "core/alg_context.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"

namespace alg_framework {

class AudioPostNode : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    (void)config;
    (void)session_ctx;
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* req_ids = req_ctx->Get<std::vector<uint64_t>>("raw_request_ids");
    auto* transcripts = req_ctx->Get<std::vector<TraceableItem<std::string>>>(
        "asr_transcripts");
    auto* slot_jsons =
        req_ctx->Get<std::vector<std::string>>("intent_slot_results");

    if (!req_ids || !transcripts || !slot_jsons) {
      req_ctx->SetError(-6401, "AudioPostNode: Missing input tensors");
      return -6401;
    }

    std::vector<AudioAsrResult> outputs(req_ids->size());
    for (size_t i = 0; i < req_ids->size(); ++i) {
      outputs[i].request_id = (*req_ids)[i];
      outputs[i].status_code = 0;
      outputs[i].transcribed_text = (*transcripts)[i].data;
      outputs[i].intent_slot_json = (*slot_jsons)[i];
    }

    req_ctx->Set("audio_final_outputs", std::move(outputs));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "AudioPostNode";
    return name;
  }
};

REGISTER_NODE(AudioPostNode);

}  // namespace alg_framework

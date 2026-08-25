#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "business/audio_asr/audio_asr_contract.h"
#include "core/alg_context.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"

namespace alg_framework {

class SlotExtractNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "SlotExtractNode";

  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    (void)config;
    (void)session_ctx;
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* transcripts = req_ctx->Get(kAsrTranscripts);
    if (!transcripts) {
      req_ctx->SetError(-6301, "SlotExtractNode: Missing asr_transcripts");
      return -6301;
    }

    std::vector<std::string> slot_jsons;
    for (const auto& item : *transcripts) {
      nlohmann::json j;
      if (item.data.find("导航") != std::string::npos) {
        j["intent"] = "NAVIGATION";
        j["slots"] = {{"destination", "清华科技园"},
                      {"avoid_toll", false},
                      {"avoid_traffic", true}};
      } else if (item.data.find("空调") != std::string::npos ||
                 item.data.find("温度") != std::string::npos) {
        j["intent"] = "VEHICLE_HVAC_CONTROL";
        j["slots"] = {{"temperature", 24}, {"fan_speed", 2}};
      } else {
        j["intent"] = "GENERAL_VOICE_CMD";
        j["slots"] = {{"raw_query", item.data}};
      }
      slot_jsons.push_back(j.dump());
    }

    req_ctx->Set(kIntentSlotResults, std::move(slot_jsons));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = kNodeType;
    return name;
  }
};

NodeDefinition MakeSlotExtractNodeDefinition() {
  NodeDefinition def;
  def.node_type = SlotExtractNode::kNodeType;
  def.category = "business";
  def.description = "Audio ASR intent and slot extraction node";
  def.inputs = {RequiredInput(kAsrTranscripts)};
  def.outputs = {Output(kIntentSlotResults)};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(SlotExtractNode, MakeSlotExtractNodeDefinition());

}  // namespace alg_framework

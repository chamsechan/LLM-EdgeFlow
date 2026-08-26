#pragma once

#include <string>
#include <vector>

#include "business/audio_asr/audio_asr_dto.h"
#include "core/blackboard_key.h"
#include "core/common_contracts.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"

namespace alg_framework {

inline constexpr char kAudioAsrBusinessName[] = "speech_audio_asr_intent_slot";

inline constexpr BlackboardKey<std::vector<AudioInputDto>> kRawAudioInputs{
    "raw_audio_inputs", "vector<AudioInputDto>"};

inline constexpr BlackboardKey<
    std::vector<TraceableItem<IAudioAsrEngine::AudioPcmData>>>
    kTraceableAudioItems{"traceable_audio_items", "traceable<AudioPcmData>[]"};

inline constexpr BlackboardKey<std::vector<TraceableItem<std::string>>>
    kAsrTranscripts{"asr_transcripts", "traceable<string>[]"};

inline constexpr BlackboardKey<std::vector<std::string>> kIntentSlotResults{
    "intent_slot_results", "vector<string>"};

inline constexpr BlackboardKey<std::vector<AudioAsrResult>> kAudioFinalOutputs{
    "audio_final_outputs", "vector<AudioAsrResult>"};

}  // namespace alg_framework

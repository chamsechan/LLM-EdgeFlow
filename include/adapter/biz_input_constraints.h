#pragma once

#include <cstddef>
#include <cstdint>

namespace llm_edgeflow::biz_input {

// Shared semantic limits; each entry point validates its own pointer/length
// representation.
inline constexpr size_t kMaxChannelNameBytes = 256;
inline constexpr int32_t kMaxAudioPcmSamples = 16000 * 60;
inline constexpr size_t kMaxAudioPcmBytes = 10 * 1024 * 1024;
inline constexpr int32_t kMinSampleRate = 8000;
inline constexpr int32_t kMaxSampleRate = 192000;

}  // namespace llm_edgeflow::biz_input

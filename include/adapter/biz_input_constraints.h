#pragma once

#include <cstddef>
#include <cstdint>

namespace llm_edgeflow::biz_input {

// 共享语义上限；各入口自行校验指针、长度和表示形式。
inline constexpr size_t kMaxTextBytes = 64 * 1024;
inline constexpr size_t kMaxDocTextBytes = 10 * 1024 * 1024;
inline constexpr size_t kMaxImageUriBytes = 4096;
inline constexpr size_t kMaxChannelNameBytes = 256;
inline constexpr int32_t kMaxAudioPcmSamples = 16000 * 60;
inline constexpr size_t kMaxAudioPcmBytes = 10 * 1024 * 1024;
inline constexpr int32_t kMinSampleRate = 8000;
inline constexpr int32_t kMaxSampleRate = 192000;
// 通用载体，不含业务字段语义。
inline constexpr size_t kMaxBufferBytes = 10 * 1024 * 1024;
inline constexpr size_t kMaxAnyBytes = 10 * 1024 * 1024;

}  // namespace llm_edgeflow::biz_input

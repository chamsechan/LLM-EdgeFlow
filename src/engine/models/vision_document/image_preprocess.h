#pragma once

#include <cstddef>
#include <string>

#include "engine/backend_interface.h"

namespace llm_edgeflow {

/**
 * @brief 校验带 stride 的 RGB8 图像，并将宽高补齐到 patch_size 的整数倍。
 * 保留原像素，将补齐区域填白，输出独立拥有的 CHW 字节数据。
 * 原图或补齐后像素数超限时返回 false，输出保持空值。
 */
bool PrepareDocumentImage(const ImageFrame& frame, int patch_size,
                          size_t max_pixels, ImageTextInput* output,
                          std::string* diagnostic = nullptr) noexcept;

}  // namespace llm_edgeflow

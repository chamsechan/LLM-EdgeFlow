#pragma once

#include <cstddef>
#include <string>

#include "engine/backend_interface.h"

namespace llm_edgeflow {

bool PrepareDocumentImage(const ImageFrame& frame, int patch_size,
                          size_t max_pixels, ImageTextInput* output,
                          std::string* diagnostic = nullptr) noexcept;

}  // namespace llm_edgeflow

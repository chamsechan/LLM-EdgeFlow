#pragma once

#include <cstddef>
#include <string>

#include "engine/backend_interface.h"

namespace llm_edgeflow {

bool DecodeDocumentImage(const std::string& path, int patch_size,
                         size_t max_pixels, ImageTextInput* output,
                         std::string* diagnostic = nullptr) noexcept;

}  // namespace llm_edgeflow

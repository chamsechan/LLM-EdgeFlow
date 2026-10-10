#include "engine/models/vision_document/image_preprocess.h"

#include <stdexcept>
#include <vector>

#include "contracts/diagnostic.h"

namespace llm_edgeflow {

bool PrepareDocumentImage(const ImageFrame& frame, int patch_size,
                          size_t max_pixels, ImageTextInput* output,
                          std::string* diagnostic) noexcept {
  if (!output) return false;
  *output = {};
  try {
    if (patch_size < 1 || patch_size > 256 || max_pixels < 1 ||
        max_pixels > 16U * 1024U * 1024U || frame.width <= 0 ||
        frame.height <= 0 ||
        static_cast<uint64_t>(frame.width) * frame.height > max_pixels ||
        frame.stride < static_cast<uint64_t>(frame.width) * 3 ||
        frame.stride > frame.data.size() / static_cast<size_t>(frame.height) ||
        frame.data.size() != frame.stride * static_cast<size_t>(frame.height)) {
      throw std::runtime_error("Invalid RGB8 frame or preprocessing limits");
    }
    const int width = frame.width;
    const int height = frame.height;
    const size_t padded_width =
        (static_cast<size_t>(width) + patch_size - 1) / patch_size * patch_size;
    const size_t padded_height =
        (static_cast<size_t>(height) + patch_size - 1) / patch_size *
        patch_size;
    if (padded_width > max_pixels / padded_height)
      throw std::runtime_error("Padded image exceeds max_pixels");
    const size_t plane = padded_width * padded_height;
    ImageTextInput staged;
    staged.width = static_cast<int>(padded_width);
    staged.height = static_cast<int>(padded_height);
    staged.patch_size = patch_size;
    staged.rgb_chw.assign(plane * 3, 255);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        for (int c = 0; c < 3; ++c) {
          staged.rgb_chw[c * plane + y * padded_width + x] =
              frame.data[static_cast<size_t>(y) * frame.stride + x * 3 + c];
        }
      }
    }
    *output = std::move(staged);
    return true;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(diagnostic, e.what());
    return false;
  } catch (...) {
    SetDiagnosticNoexcept(diagnostic, "Unknown image preprocessing error");
    return false;
  }
}

}  // namespace llm_edgeflow

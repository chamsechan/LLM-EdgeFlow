#include "engine/models/vision_document/image_decode.h"

#include <fstream>
#include <memory>
#include <vector>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNM
#include <stb_image.h>

namespace llm_edgeflow {

bool DecodeDocumentImage(const std::string& path, int patch_size,
                         size_t max_pixels, ImageTextInput* output,
                         std::string* diagnostic) noexcept {
  if (!output) return false;
  *output = {};
  try {
    if (path.empty() || path.find('\0') != std::string::npos ||
        patch_size < 1 || patch_size > 256 || max_pixels < 1 ||
        max_pixels > 16U * 1024U * 1024U) {
      throw std::runtime_error("Invalid image path or decode limits");
    }
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open image: " + path);
    const auto length = file.tellg();
    if (length <= 0 || length > 32 * 1024 * 1024) {
      throw std::runtime_error("Image file is empty or exceeds 32 MiB");
    }
    std::vector<unsigned char> encoded(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(encoded.data()), length)) {
      throw std::runtime_error("Cannot read image bytes");
    }
    int width = 0, height = 0, channels = 0;
    if (!stbi_info_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                               &width, &height, &channels) ||
        width <= 0 || height <= 0 ||
        static_cast<uint64_t>(width) * height > max_pixels) {
      throw std::runtime_error("Invalid image or image exceeds max_pixels");
    }
    const size_t padded_width =
        (static_cast<size_t>(width) + patch_size - 1) / patch_size * patch_size;
    const size_t padded_height =
        (static_cast<size_t>(height) + patch_size - 1) / patch_size *
        patch_size;
    const size_t plane = padded_width * padded_height;
    if (plane > max_pixels)
      throw std::runtime_error("Padded image exceeds max_pixels");
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> rgb(
        stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                              &width, &height, &channels, 3),
        stbi_image_free);
    if (!rgb) throw std::runtime_error("Image decode failed");
    ImageTextInput staged;
    staged.width = static_cast<int>(padded_width);
    staged.height = static_cast<int>(padded_height);
    staged.patch_size = patch_size;
    staged.rgb_chw.assign(plane * 3, 255);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        for (int c = 0; c < 3; ++c) {
          staged.rgb_chw[c * plane + y * padded_width + x] =
              rgb.get()[(static_cast<size_t>(y) * width + x) * 3 + c];
        }
      }
    }
    *output = std::move(staged);
    return true;
  } catch (const std::exception& e) {
    inference_detail::SetDiagnostic(diagnostic, e.what());
    return false;
  } catch (...) {
    inference_detail::SetDiagnostic(diagnostic, "Unknown image decode error");
    return false;
  }
}

}  // namespace llm_edgeflow

#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"
#include "platform_mock/operator_data_types.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNM
#include <stb_image.h>

namespace alg_demo {
namespace {

int BuildImageQueryRequests(
    const DemoOptions& options,
    const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs,
    DemoRequestBatch* out) {
  if (!out || inputs.size() != 2) return 3;

  std::unordered_map<std::string, std::vector<std::string>> sections;
  std::string err;
  if (!ParseTagSections(options.dataset_path, &sections, &err)) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[OcrInvoiceQaDemo ERROR] " << err << std::endl;
      return 4;
    }
  }

  struct Storage {
    std::string image;
    std::string prompt;
    std::vector<uint8_t> pixels;
    CompanyFrame frame{};
    CompanyString prompt_str{};
  };
  auto storage = std::make_shared<Storage>();
  if (!sections["IMAGE"].empty()) storage->image = sections["IMAGE"][0];
  if (!sections["PROMPT"].empty()) storage->prompt = sections["PROMPT"][0];
  if (storage->image.empty() || storage->prompt.empty()) {
    if (options.allow_fallback_sample) {
      std::cout << "[OcrInvoiceQaDemo WARN] Dataset sections missing, using "
                   "fallback sample."
                << std::endl;
      if (storage->image.empty())
        storage->image = "data/kite_invoice_sample.png";
      if (storage->prompt.empty()) {
        storage->prompt = "提取发票代码、号码与总金额";
      }
    } else {
      std::cerr
          << "[OcrInvoiceQaDemo ERROR] Dataset missing required [IMAGE] or "
             "[PROMPT] sections."
          << std::endl;
      return 4;
    }
  }

  // Dataset files are only a Demo carrier source. The SDK receives RGB8 pixels.
  std::ifstream file(storage->image, std::ios::binary | std::ios::ate);
  const auto length = file ? file.tellg() : std::streampos(-1);
  if (length <= 0 || length > 32 * 1024 * 1024) {
    std::cerr << "[OcrInvoiceQaDemo ERROR] Cannot read image or image exceeds "
                 "32 MiB: "
              << storage->image << std::endl;
    return 4;
  }
  std::vector<uint8_t> encoded(static_cast<size_t>(length));
  file.seekg(0);
  if (!file.read(reinterpret_cast<char*>(encoded.data()), length)) return 4;
  int width = 0, height = 0, channels = 0;
  if (!stbi_info_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                             &width, &height, &channels) ||
      width <= 0 || height <= 0 ||
      static_cast<uint64_t>(width) * height > 16U * 1024U * 1024U) {
    std::cerr << "[OcrInvoiceQaDemo ERROR] Invalid or oversized image"
              << std::endl;
    return 4;
  }
  std::unique_ptr<uint8_t, decltype(&stbi_image_free)> rgb(
      stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                            &width, &height, &channels, 3),
      stbi_image_free);
  if (!rgb) {
    std::cerr << "[OcrInvoiceQaDemo ERROR] Image decode failed" << std::endl;
    return 4;
  }
  storage->pixels.assign(rgb.get(),
                         rgb.get() + static_cast<size_t>(width) * height * 3);
  storage->frame = {inputs[0].service_type.value_or(0),
                    height,
                    width,
                    width * 3,
                    storage->pixels.data(),
                    nullptr};
  storage->prompt_str = {static_cast<int32_t>(storage->prompt.size()),
                         const_cast<char*>(storage->prompt.data())};

  DemoRequestBatch batch;
  batch.requests.resize(1);
  batch.requests[0]["demo." + inputs[0].type] =
      llm_edgeflow::operator_api::MakeBorrowedOperatorInput(&storage->frame);
  batch.requests[0]["demo." + inputs[1].type] =
      llm_edgeflow::operator_api::MakeBorrowedOperatorInput(
          &storage->prompt_str);
  batch.storage = storage;
  *out = std::move(batch);
  return 0;
}

REGISTER_DEMO_INPUT("CompanyFrame,CompanyString", BuildImageQueryRequests);

}  // namespace
}  // namespace alg_demo

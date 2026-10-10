#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"
#include "platform_mock/operator_data_types.h"

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
    CompanyString image_str{};
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
      if (storage->image.empty()) storage->image = "./data/invoice_01.jpg";
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

  storage->image_str = {static_cast<int32_t>(storage->image.size()),
                        const_cast<char*>(storage->image.data())};
  storage->frame = {60001, inputs[0].service_type.value_or(0),
                    &storage->image_str, nullptr};
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

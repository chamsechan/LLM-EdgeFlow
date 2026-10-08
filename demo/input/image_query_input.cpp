#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"

namespace alg_demo {
namespace {

// 数据集：[IMAGE] / [PROMPT] 标签段，各取第一段。
// 两个输入项按 io.input 顺序依次是图像帧、提问文本。
struct ImageQueryStorage {
  std::string image;
  std::string prompt;
  CompanyString image_str{};
  CompanyString prompt_str{};
  CompanyFrame frame{};
};

int BuildImageQueryRequests(
    const DemoOptions& options,
    const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs,
    DemoRequestBatch* out) {
  if (inputs.size() != 2) {
    std::cerr << "[ImageQueryInput ERROR] Expected 2 input items (frame, "
                 "string), got "
              << inputs.size() << std::endl;
    return 3;
  }

  std::unordered_map<std::string, std::vector<std::string>> sections;
  std::string err;
  if (!ParseTagSections(options.dataset_path, &sections, &err)) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[ImageQueryInput ERROR] " << err << std::endl;
      return 4;
    }
  }

  std::string image;
  std::string prompt;
  if (!sections["IMAGE"].empty()) image = sections["IMAGE"][0];
  if (!sections["PROMPT"].empty()) prompt = sections["PROMPT"][0];

  if (image.empty() || prompt.empty()) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[ImageQueryInput ERROR] Dataset missing required [IMAGE] "
                   "or [PROMPT] sections."
                << std::endl;
      return 4;
    }
    std::cout << "[ImageQueryInput WARN] Dataset sections missing, using "
                 "fallback sample."
              << std::endl;
    if (image.empty()) image = "./data/invoice_01.jpg";
    if (prompt.empty()) prompt = "提取发票代码、号码与总金额";
  }

  auto storage = std::make_shared<ImageQueryStorage>();
  storage->image = std::move(image);
  storage->prompt = std::move(prompt);
  storage->image_str = BorrowCompanyString(storage->image);
  storage->prompt_str = BorrowCompanyString(storage->prompt);
  storage->frame = {60001, &storage->image_str, nullptr};

  out->requests.assign(1, {});
  out->requests[0][DemoIoKey(inputs[0])] =
      llm_edgeflow::operator_api::MakeBorrowedOperatorInput(&storage->frame);
  out->requests[0][DemoIoKey(inputs[1])] =
      llm_edgeflow::operator_api::MakeBorrowedOperatorInput(
          &storage->prompt_str);
  out->request_info.assign(1, nlohmann::json::object());
  out->request_info[0]["prompt"] = storage->prompt;
  out->storage = std::move(storage);
  return 0;
}

REGISTER_DEMO_INPUT("CompanyFrame,CompanyString", BuildImageQueryRequests);

}  // namespace
}  // namespace alg_demo

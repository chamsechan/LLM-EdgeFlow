#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"

namespace alg_demo {
namespace {

// 数据集：每行一条文本。
struct KeywordStorage {
  std::vector<std::string> lines;
  std::vector<CompanyString> texts;
  std::vector<CompanyOperatorKeywordInput> inputs;
};

std::vector<std::string> FallbackLines() {
  return {"请帮我联系一下VIP专员，我有一笔大客户加急订单需要优先处理。",
          "今天天气真不错，阳光明媚，我想去公园散散步。"};
}

int BuildKeywordRequests(
    const DemoOptions& options,
    const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs,
    DemoRequestBatch* out) {
  std::vector<std::string> lines;
  std::string err;
  if (!ReadLinesFromFile(options.dataset_path, &lines, &err)) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[KeywordInput ERROR] " << err << std::endl;
      return 4;
    }
    std::cout << "[KeywordInput WARN] Dataset not found, using fallback sample."
              << std::endl;
    lines = FallbackLines();
  }
  if (lines.empty()) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[KeywordInput ERROR] Dataset file has no valid lines."
                << std::endl;
      return 4;
    }
    lines = FallbackLines();
  }

  auto storage = std::make_shared<KeywordStorage>();
  storage->lines = std::move(lines);
  const size_t count = storage->lines.size();
  storage->texts.reserve(count);
  storage->inputs.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    storage->texts.push_back(BorrowCompanyString(storage->lines[i]));
    storage->inputs.push_back(
        {static_cast<uint64_t>(20001 + i), &storage->texts.back()});
  }

  const std::string key = DemoIoKey(inputs[0]);
  out->requests.assign(count, {});
  out->request_info.assign(count, nlohmann::json::object());
  for (size_t i = 0; i < count; ++i) {
    out->requests[i][key] =
        llm_edgeflow::operator_api::MakeBorrowedOperatorInput(
            &storage->inputs[i]);
    out->request_info[i]["text"] = storage->lines[i];
  }
  out->storage = std::move(storage);
  return 0;
}

REGISTER_DEMO_INPUT("CompanyOperatorKeywordInput", BuildKeywordRequests);

}  // namespace
}  // namespace alg_demo

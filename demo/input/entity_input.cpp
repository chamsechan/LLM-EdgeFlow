#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"

namespace alg_demo {
namespace {

// 数据集：每行一条文本（纯文本或 JSON 请求）。同一结构上的多个业务共用本构造。
struct EntityStorage {
  std::vector<std::string> lines;
  std::vector<CompanyString> texts;
  std::vector<CompanyOperatorEntityInput> inputs;
};

std::vector<std::string> FallbackLines() {
  return {
      "张三在清华大学毕业后加入了一家北京的人工智能公司，作为算法工程师负责"
      "NPU芯片和深度学习大模型的研发项目。"};
}

int BuildEntityRequests(
    const DemoOptions& options,
    const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs,
    DemoRequestBatch* out) {
  std::vector<std::string> lines;
  std::string err;
  if (!ReadLinesFromFile(options.dataset_path, &lines, &err)) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[EntityInput ERROR] " << err << std::endl;
      return 4;
    }
    std::cout << "[EntityInput WARN] Dataset not found, using fallback sample."
              << std::endl;
    lines = FallbackLines();
  }
  if (lines.empty()) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[EntityInput ERROR] Dataset file has no valid lines."
                << std::endl;
      return 4;
    }
    lines = FallbackLines();
  }

  auto storage = std::make_shared<EntityStorage>();
  storage->lines = std::move(lines);
  const size_t count = storage->lines.size();
  storage->texts.reserve(count);
  const int32_t service_type = DemoServiceType(inputs[0]);
  storage->inputs.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    storage->texts.push_back(BorrowCompanyString(storage->lines[i]));
    storage->inputs.push_back({static_cast<uint64_t>(30001 + i),
                               &storage->texts.back(), service_type});
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

REGISTER_DEMO_INPUT("CompanyOperatorEntityInput", BuildEntityRequests);

}  // namespace
}  // namespace alg_demo

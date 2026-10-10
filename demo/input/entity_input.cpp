#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"
#include "platform_mock/operator_data_types.h"

namespace alg_demo {
namespace {

int BuildEntityRequests(
    const DemoOptions& options,
    const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs,
    DemoRequestBatch* out) {
  if (!out || inputs.size() != 1) return 3;

  struct Storage {
    std::vector<std::string> lines;
    std::vector<CompanyString> text_strs;
    std::vector<CompanyOperatorEntityInput> carriers;
  };
  auto storage = std::make_shared<Storage>();
  std::string err;
  if (!ReadLinesFromFile(options.dataset_path, &storage->lines, &err)) {
    if (options.allow_fallback_sample) {
      std::cout << "[EntityExtractDemo WARN] Dataset not found, using fallback "
                   "sample."
                << std::endl;
      storage->lines = {
          "张三在清华大学毕业后加入了一家北京的人工智能公司，作为算法工程师负责"
          "NPU芯片和深度学习大模型的研发项目。"};
    } else {
      std::cerr << "[EntityExtractDemo ERROR] " << err << std::endl;
      return 4;
    }
  }

  if (storage->lines.empty()) {
    if (options.allow_fallback_sample) {
      storage->lines = {
          "张三在清华大学毕业后加入了一家北京的人工智能公司，作为算法工程师负责"
          "NPU芯片和深度学习大模型的研发项目。"};
    } else {
      std::cerr << "[EntityExtractDemo ERROR] Dataset file has no valid lines."
                << std::endl;
      return 4;
    }
  }

  storage->text_strs.reserve(storage->lines.size());
  storage->carriers.reserve(storage->lines.size());
  for (size_t i = 0; i < storage->lines.size(); ++i) {
    storage->text_strs.push_back(
        {static_cast<int32_t>(storage->lines[i].size()),
         const_cast<char*>(storage->lines[i].data())});
    storage->carriers.push_back({static_cast<uint64_t>(30001 + i),
                                 inputs[0].service_type.value_or(0),
                                 &storage->text_strs.back()});
  }

  DemoRequestBatch batch;
  batch.requests.resize(storage->carriers.size());
  for (size_t i = 0; i < storage->carriers.size(); ++i) {
    batch.requests[i]["demo." + inputs[0].type] =
        llm_edgeflow::operator_api::MakeBorrowedOperatorInput(
            &storage->carriers[i]);
  }
  batch.storage = storage;
  *out = std::move(batch);
  return 0;
}

REGISTER_DEMO_INPUT("CompanyOperatorEntityInput", BuildEntityRequests);

}  // namespace
}  // namespace alg_demo

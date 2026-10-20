#include <cstdint>
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

// 宿主载体相同，回退样例仍按业务区分。
std::vector<std::string> FallbackSamples(const std::string& name) {
  if (name == "keyword_match")
    return {"请帮我联系一下VIP专员，我有一笔大客户加急订单需要优先处理。",
            "今天天气真不错，阳光明媚，我想去公园散散步。"};
  return {
      "张三在清华大学毕业后加入了一家北京的人工智能公司，作为算法工程师负责"
      "NPU芯片和深度学习大模型的研发项目。"};
}

int BuildTextRequests(
    const DemoOptions& options,
    const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs,
    DemoRequestBatch* out) {
  if (!out || inputs.size() != 1) return 3;

  struct Storage {
    std::vector<std::string> lines;
    std::vector<MockTextInput> texts;
    std::vector<MockDataPool> pools;
    std::vector<MockInput> carriers;
  };
  auto storage = std::make_shared<Storage>();
  std::string err;
  if (!ReadLinesFromFile(options.dataset_path, &storage->lines, &err)) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[TextDemo ERROR] " << err << std::endl;
      return 4;
    }
    std::cout << "[TextDemo WARN] Dataset not found, using fallback sample."
              << std::endl;
    storage->lines = FallbackSamples(inputs[0].name);
  }
  if (storage->lines.empty()) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[TextDemo ERROR] Dataset file has no valid lines."
                << std::endl;
      return 4;
    }
    storage->lines = FallbackSamples(inputs[0].name);
  }

  const size_t count = storage->lines.size();
  storage->texts.resize(count);
  storage->pools.resize(count);
  storage->carriers.resize(count);
  DemoRequestBatch batch;
  batch.requests.resize(count);
  for (size_t i = 0; i < count; ++i) {
    storage->texts[i] = {static_cast<int32_t>(sizeof(MockTextInput)),
                         storage->lines[i].data(),
                         static_cast<int32_t>(storage->lines[i].size())};
    storage->pools[i] = {static_cast<int32_t>(sizeof(MockDataPool)),
                         kMockDataText, &storage->texts[i]};
    storage->carriers[i] = {static_cast<int32_t>(sizeof(MockInput)),
                            kMockInputMultidata, 1, &storage->pools[i]};
    batch.requests[i]["demo." + inputs[0].type] =
        llm_edgeflow::operator_api::MakeBorrowedOperatorInput(
            &storage->carriers[i]);
  }
  batch.storage = storage;
  *out = std::move(batch);
  return 0;
}

REGISTER_DEMO_INPUT("MockInput", BuildTextRequests);

}  // namespace
}  // namespace alg_demo

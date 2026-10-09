#include <algorithm>
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

int BuildAuditRequests(
    const DemoOptions& options,
    const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs,
    DemoRequestBatch* out) {
  if (!out || inputs.size() != 1) return 3;

  std::unordered_map<std::string, std::vector<std::string>> sections;
  std::string err;
  if (!ParseTagSections(options.dataset_path, &sections, &err)) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[DialogueAuditDemo ERROR] " << err << std::endl;
      return 4;
    }
  }

  struct Storage {
    std::vector<std::string> channels;
    std::vector<std::string> dialogues;
    std::vector<CompanyString> channel_strs;
    std::vector<CompanyString> dialogue_strs;
    std::vector<CompanyOperatorAuditInput> carriers;
  };
  auto storage = std::make_shared<Storage>();
  storage->channels = sections["CHANNEL"];
  storage->dialogues = sections["DIALOGUE"];
  if (storage->channels.empty() || storage->dialogues.empty()) {
    if (options.allow_fallback_sample) {
      std::cout << "[DialogueAuditDemo WARN] Dataset sections missing, using "
                   "fallback sample."
                << std::endl;
      storage->channels = {"VIP专席客服", "在线售后IM"};
      storage->dialogues = {
          "亲，平台退款审核太慢了，你加我私人微信转账给我吧，我私下把商品寄给你"
          "，"
          "还能返现20元！",
          "您好，您的商品符合7天无理由退货政策，已为您在系统提交退款换货流程，"
          "请保持手机畅通。"};
    } else {
      std::cerr << "[DialogueAuditDemo ERROR] Dataset missing [CHANNEL] or "
                   "[DIALOGUE] sections."
                << std::endl;
      return 4;
    }
  }

  const size_t count =
      std::min(storage->channels.size(), storage->dialogues.size());
  storage->channel_strs.reserve(count);
  storage->dialogue_strs.reserve(count);
  storage->carriers.reserve(count);
  DemoRequestBatch batch;
  batch.request_info.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    storage->channel_strs.push_back(
        {static_cast<int32_t>(storage->channels[i].size()),
         const_cast<char*>(storage->channels[i].data())});
    storage->dialogue_strs.push_back(
        {static_cast<int32_t>(storage->dialogues[i].size()),
         const_cast<char*>(storage->dialogues[i].data())});
    storage->carriers.push_back({static_cast<uint64_t>(40001 + i),
                                 &storage->dialogue_strs.back(),
                                 &storage->channel_strs.back()});
    batch.request_info.push_back({{"channel", storage->channels[i]}});
  }

  batch.requests.resize(count);
  for (size_t i = 0; i < count; ++i) {
    batch.requests[i]["demo." + inputs[0].type] =
        llm_edgeflow::operator_api::MakeBorrowedOperatorInput(
            &storage->carriers[i]);
  }
  batch.storage = storage;
  *out = std::move(batch);
  return 0;
}

REGISTER_DEMO_INPUT("CompanyOperatorAuditInput", BuildAuditRequests);

}  // namespace
}  // namespace alg_demo

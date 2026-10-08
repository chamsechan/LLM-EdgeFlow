#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"

namespace alg_demo {
namespace {

// 数据集：[CHANNEL] / [DIALOGUE] 标签段，按序号配对。
struct AuditStorage {
  std::vector<std::string> channels;
  std::vector<std::string> dialogues;
  std::vector<CompanyString> channel_strs;
  std::vector<CompanyString> dialogue_strs;
  std::vector<CompanyOperatorAuditInput> inputs;
};

int BuildAuditRequests(
    const DemoOptions& options,
    const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs,
    DemoRequestBatch* out) {
  std::unordered_map<std::string, std::vector<std::string>> sections;
  std::string err;
  if (!ParseTagSections(options.dataset_path, &sections, &err)) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[AuditInput ERROR] " << err << std::endl;
      return 4;
    }
  }

  auto channels = sections["CHANNEL"];
  auto dialogues = sections["DIALOGUE"];
  if (channels.empty() || dialogues.empty()) {
    if (!options.allow_fallback_sample) {
      std::cerr << "[AuditInput ERROR] Dataset missing [CHANNEL] or "
                   "[DIALOGUE] sections."
                << std::endl;
      return 4;
    }
    std::cout << "[AuditInput WARN] Dataset sections missing, using fallback "
                 "sample."
              << std::endl;
    channels = {"VIP专席客服", "在线售后IM"};
    dialogues = {
        "亲，平台退款审核太慢了，你加我私人微信转账给我吧，我私下把商品寄给你"
        "，"
        "还能返现20元！",
        "您好，您的商品符合7天无理由退货政策，已为您在系统提交退款换货流程，"
        "请保持手机畅通。"};
  }

  auto storage = std::make_shared<AuditStorage>();
  storage->channels = std::move(channels);
  storage->dialogues = std::move(dialogues);
  const size_t count =
      std::min(storage->channels.size(), storage->dialogues.size());
  storage->channel_strs.reserve(count);
  storage->dialogue_strs.reserve(count);
  const int32_t service_type = DemoServiceType(inputs[0]);
  storage->inputs.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    storage->channel_strs.push_back(BorrowCompanyString(storage->channels[i]));
    storage->dialogue_strs.push_back(
        BorrowCompanyString(storage->dialogues[i]));
    storage->inputs.push_back({static_cast<uint64_t>(40001 + i),
                               &storage->dialogue_strs.back(),
                               &storage->channel_strs.back(), service_type});
  }

  const std::string key = DemoIoKey(inputs[0]);
  out->requests.assign(count, {});
  out->request_info.assign(count, nlohmann::json::object());
  for (size_t i = 0; i < count; ++i) {
    out->requests[i][key] =
        llm_edgeflow::operator_api::MakeBorrowedOperatorInput(
            &storage->inputs[i]);
    out->request_info[i]["channel"] = storage->channels[i];
    out->request_info[i]["dialogue"] = storage->dialogues[i];
  }
  out->storage = std::move(storage);
  return 0;
}

REGISTER_DEMO_INPUT("CompanyOperatorAuditInput", BuildAuditRequests);

}  // namespace
}  // namespace alg_demo

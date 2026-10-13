#include <vector>

#include "demo/common/demo_io_registry.h"
#include "demo/common/text_requests.h"
#include "platform_mock/operator_data_types.h"

namespace alg_demo {
namespace {

int BuildKeywordRequests(
    const DemoOptions& options,
    const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs,
    DemoRequestBatch* out) {
  return BuildTextRequests<CompanyOperatorKeywordInput>(
      options, inputs, out, "KeywordMatchDemo",
      {"请帮我联系一下VIP专员，我有一笔大客户加急订单需要优先处理。",
       "今天天气真不错，阳光明媚，我想去公园散散步。"});
}

REGISTER_DEMO_INPUT("CompanyOperatorKeywordInput", BuildKeywordRequests);

}  // namespace
}  // namespace alg_demo

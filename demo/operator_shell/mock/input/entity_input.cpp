#include <vector>

#include "demo/common/demo_io_registry.h"
#include "demo/operator_shell/mock/text_requests.h"
#include "platform_mock/operator_data_types.h"

namespace alg_demo {
namespace {

int BuildEntityRequests(
    const DemoOptions& options,
    const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs,
    DemoRequestBatch* out) {
  return BuildTextRequests<CompanyOperatorEntityInput>(
      options, inputs, out, "EntityExtractDemo",
      {"张三在清华大学毕业后加入了一家北京的人工智能公司，作为算法工程师负责"
       "NPU芯片和深度学习大模型的研发项目。"});
}

REGISTER_DEMO_INPUT("CompanyOperatorEntityInput", BuildEntityRequests);

}  // namespace
}  // namespace alg_demo

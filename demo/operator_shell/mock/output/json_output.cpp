#include "demo/common/demo_io_registry.h"
#include "platform_mock/operator_data_types.h"

namespace alg_demo {
namespace {

void ShowJsonResult(const void* output, const nlohmann::json&, int32_t* status,
                    nlohmann::json* sample_output) {
  const auto& out = *static_cast<const CompanyOperatorOutput*>(output);
  *status = out.status_code;
  const auto* json = out.result_json;
  if (json && json->length > 0) {
    *sample_output =
        nlohmann::json::parse(json->data, json->data + json->length);
  }
}

REGISTER_DEMO_OUTPUT("CompanyOperatorOutput", ShowJsonResult);

}  // namespace
}  // namespace alg_demo

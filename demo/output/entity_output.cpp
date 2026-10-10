#include <string>

#include "demo/common/demo_io_registry.h"
#include "platform_mock/operator_data_types.h"

namespace alg_demo {
namespace {

void ShowEntityResult(const void* output, const nlohmann::json&,
                      int32_t* status, nlohmann::json* sample_output) {
  const auto& out = *static_cast<const CompanyOperatorEntityOutput*>(output);
  *status = out.status_code;
  std::string entities_json;
  if (out.entities_json && out.entities_json->data) {
    entities_json.assign(out.entities_json->data, out.entities_json->length);
  }
  if (!entities_json.empty()) {
    auto parsed = nlohmann::json::parse(entities_json, nullptr, false);
    if (parsed.is_discarded()) {
      (*sample_output)["entities_raw"] = entities_json;
    } else {
      (*sample_output)["entities"] = parsed;
    }
  }
}

REGISTER_DEMO_OUTPUT("CompanyOperatorEntityOutput", ShowEntityResult);

}  // namespace
}  // namespace alg_demo

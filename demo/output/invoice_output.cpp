#include <string>

#include "demo/common/demo_io_registry.h"
#include "platform_mock/operator_data_types.h"

namespace alg_demo {
namespace {

void ShowInvoiceResult(const void* output, const nlohmann::json&,
                       int32_t* status, nlohmann::json* sample_output) {
  const auto& out = *static_cast<const CompanyOdOutput*>(output);
  *status = out.status_code;
  (*sample_output)["detected_box_count"] = out.detected_box_count;
  std::string result_json;
  if (out.result_json && out.result_json->data) {
    result_json.assign(out.result_json->data, out.result_json->length);
  }
  if (!result_json.empty()) {
    auto parsed = nlohmann::json::parse(result_json, nullptr, false);
    if (parsed.is_discarded()) {
      (*sample_output)["extracted_invoice_raw"] = result_json;
    } else {
      (*sample_output)["extracted_invoice"] = parsed;
    }
  }
}

REGISTER_DEMO_OUTPUT("CompanyOdOutput", ShowInvoiceResult);

}  // namespace
}  // namespace alg_demo

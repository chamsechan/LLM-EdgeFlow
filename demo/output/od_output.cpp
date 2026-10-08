#include <iostream>
#include <string>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"

namespace alg_demo {
namespace {

void ShowOdResult(const void* output, const nlohmann::json& /*request_info*/,
                  uint64_t* request_id, int32_t* status,
                  nlohmann::json* sample_output) {
  const auto& out = *static_cast<const CompanyOdOutput*>(output);
  const std::string result_json = CopyCompanyString(out.result_json);

  PrintDivider();
  std::cout << "  Request ID     : " << out.request_id << "\n"
            << "  Status Code    : " << out.status_code << "\n"
            << "  OCR Box Count  : " << out.detected_box_count << "\n"
            << "  Extracted JSON : " << result_json << std::endl;

  *request_id = out.request_id;
  *status = out.status_code;
  (*sample_output)["detected_box_count"] = out.detected_box_count;
  if (!result_json.empty()) {
    auto parsed = nlohmann::json::parse(result_json, nullptr, false);
    if (parsed.is_discarded()) {
      (*sample_output)["extracted_invoice_raw"] = result_json;
    } else {
      (*sample_output)["extracted_invoice"] = parsed;
    }
  }
}

REGISTER_DEMO_OUTPUT("CompanyOdOutput", ShowOdResult);

}  // namespace
}  // namespace alg_demo

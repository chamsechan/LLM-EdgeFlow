#include <string>

#include "demo/common/demo_io_registry.h"
#include "platform_mock/operator_data_types.h"

namespace alg_demo {
namespace {

void ShowKeywordResult(const void* output, const nlohmann::json&,
                       int32_t* status, nlohmann::json* sample_output) {
  const auto& out = *static_cast<const CompanyOperatorKeywordOutput*>(output);
  *status = out.status_code;
  (*sample_output)["is_hit"] = out.is_hit != 0;
  std::string match_result_json;
  if (out.match_result_json && out.match_result_json->data) {
    match_result_json.assign(out.match_result_json->data,
                             out.match_result_json->length);
  }
  if (!match_result_json.empty()) {
    auto parsed = nlohmann::json::parse(match_result_json, nullptr, false);
    if (parsed.is_discarded()) {
      (*sample_output)["match_result_raw"] = match_result_json;
    } else {
      (*sample_output)["match_result"] = parsed;
    }
  }
}

REGISTER_DEMO_OUTPUT("CompanyOperatorKeywordOutput", ShowKeywordResult);

}  // namespace
}  // namespace alg_demo

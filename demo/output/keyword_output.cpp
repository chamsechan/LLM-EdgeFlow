#include <iostream>
#include <string>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"

namespace alg_demo {
namespace {

void ShowKeywordResult(const void* output, const nlohmann::json& request_info,
                       uint64_t* request_id, int32_t* status,
                       nlohmann::json* sample_output) {
  const auto& out = *static_cast<const CompanyOperatorKeywordOutput*>(output);
  const std::string match_result_json =
      CopyCompanyString(out.match_result_json);

  PrintDivider();
  if (request_info.contains("text")) {
    std::cout << "  Input      : \"" << request_info["text"].get<std::string>()
              << "\"\n";
  }
  std::cout << "  Request ID : " << out.request_id << "\n"
            << "  Is Hit     : " << (out.is_hit ? "YES (命中)" : "NO (未命中)")
            << "\n"
            << "  JSON Output: " << match_result_json << std::endl;

  *request_id = out.request_id;
  *status = out.status_code;
  (*sample_output)["is_hit"] = (out.is_hit != 0);
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

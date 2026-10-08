#include <iostream>
#include <string>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"

namespace alg_demo {
namespace {

void ShowEntityResult(const void* output, const nlohmann::json& request_info,
                      uint64_t* request_id, int32_t* status,
                      nlohmann::json* sample_output) {
  const auto& out = *static_cast<const CompanyOperatorEntityOutput*>(output);
  const std::string entities_json = CopyCompanyString(out.entities_json);

  PrintDivider();
  if (request_info.contains("text")) {
    std::cout << "  Input Sentence : \""
              << request_info["text"].get<std::string>() << "\"\n";
  }
  std::cout << "  Request ID     : " << out.request_id << "\n"
            << "  Extracted JSON : " << entities_json << std::endl;

  *request_id = out.request_id;
  *status = out.status_code;
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

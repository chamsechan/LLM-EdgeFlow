#include <iostream>
#include <string>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"

namespace alg_demo {
namespace {

void ShowAudioResult(const void* output, const nlohmann::json& request_info,
                     uint64_t* request_id, int32_t* status,
                     nlohmann::json* sample_output) {
  const auto& out = *static_cast<const CompanyOperatorAudioOutput*>(output);
  const std::string transcribed_text = CopyCompanyString(out.transcribed_text);
  const std::string intent_slot_json = CopyCompanyString(out.intent_slot_json);

  PrintDivider();
  std::cout << "  Request ID     : " << out.request_id << "\n"
            << "  ASR Text       : " << transcribed_text << "\n"
            << "  Intent / Slots : " << intent_slot_json << std::endl;
  if (request_info.contains("reference_text")) {
    std::cout << "  Reference Text : "
              << request_info["reference_text"].get<std::string>() << "\n"
              << "  Expected Cat   : "
              << request_info.value("expected_category", std::string())
              << std::endl;
  }

  *request_id = out.request_id;
  *status = out.status_code;
  (*sample_output)["transcribed_text"] = transcribed_text;
  if (!intent_slot_json.empty()) {
    auto parsed = nlohmann::json::parse(intent_slot_json, nullptr, false);
    if (parsed.is_discarded()) {
      (*sample_output)["intent_slot_raw"] = intent_slot_json;
    } else {
      (*sample_output)["intent_slot"] = parsed;
    }
  }
}

REGISTER_DEMO_OUTPUT("CompanyOperatorAudioOutput", ShowAudioResult);

}  // namespace
}  // namespace alg_demo

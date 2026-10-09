#include <string>

#include "demo/common/demo_io_registry.h"
#include "platform_mock/operator_data_types.h"

namespace alg_demo {
namespace {

void ShowAudioResult(const void* output, const nlohmann::json&,
                     uint64_t* request_id, int32_t* status,
                     nlohmann::json* sample_output) {
  const auto& out = *static_cast<const CompanyOperatorAudioOutput*>(output);
  *request_id = out.request_id;
  *status = out.status_code;
  std::string transcribed_text;
  std::string intent_slot_json;
  if (out.transcribed_text && out.transcribed_text->data) {
    transcribed_text.assign(out.transcribed_text->data,
                            out.transcribed_text->length);
  }
  if (out.intent_slot_json && out.intent_slot_json->data) {
    intent_slot_json.assign(out.intent_slot_json->data,
                            out.intent_slot_json->length);
  }
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

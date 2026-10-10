#include <string>

#include "demo/common/demo_io_registry.h"
#include "platform_mock/operator_data_types.h"

namespace alg_demo {
namespace {

void ShowDocResult(const void* output, const nlohmann::json&, int32_t* status,
                   nlohmann::json* sample_output) {
  const auto& out = *static_cast<const CompanyOperatorDocOutput*>(output);
  *status = out.status_code;
  std::string intent_name;
  std::string answer_text;
  if (out.intent_name && out.intent_name->data) {
    intent_name.assign(out.intent_name->data, out.intent_name->length);
  }
  if (out.answer_text && out.answer_text->data) {
    answer_text.assign(out.answer_text->data, out.answer_text->length);
  }
  (*sample_output)["chunk_count"] = out.chunk_count;
  (*sample_output)["intent_name"] = intent_name;
  (*sample_output)["confidence"] = out.confidence;
  (*sample_output)["answer_text"] = answer_text;
}

REGISTER_DEMO_OUTPUT("CompanyOperatorDocOutput", ShowDocResult);

}  // namespace
}  // namespace alg_demo
